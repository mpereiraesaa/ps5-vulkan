#include "vk_queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static VkResult image_requirements(VkDevice d, const VkImageCreateInfo *i, VkMemoryRequirements *r)
{ (void)d; (void)i; *r = (VkMemoryRequirements){256, 256, 1}; return VK_SUCCESS; }
static VkResult memory_allocate(void *ctx, VkDeviceSize n, void **a, void **b)
{ (void)ctx; *a = *b = calloc(1, n); return *a ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void memory_release(void *ctx, void *b) { (void)ctx; free(b); }
static VkResult memory_sync(void *ctx, void *b, VkDeviceSize offset, VkDeviceSize bytes)
{ (void)ctx; (void)b; (void)offset; (void)bytes; return VK_SUCCESS; }

struct fixture {
    unsigned prepares, launches, polls, releases;
    VkResult prepare_result, launch_result, poll_result;
    uint64_t serial, now;
    int complete, wrong;
};
static VkResult prepare(VkDevice d, const struct ps5vk_submission *s, void **job)
{
    struct fixture *f = d->progress.context; ++f->prepares;
    assert(s->buffers[0]->state == PS5VK_EXECUTABLE);
    if (f->prepare_result) return f->prepare_result;
    f->serial = s->serial; *job = f; return VK_SUCCESS;
}
static VkResult launch(VkDevice d, void *job)
{
    struct fixture *f = job; ++f->launches; f->complete = 0;
    assert(d->submission && d->submission->buffers[0]->state == PS5VK_PENDING);
    return f->launch_result;
}
static VkResult poll_backend(VkDevice d, void *job, uint64_t *completed)
{
    (void)d; struct fixture *f = job; ++f->polls;
    *completed = f->complete ? f->serial + !!f->wrong : 0;
    return f->poll_result;
}
static void release(VkDevice d, void *job)
{
    struct fixture *f = job; ++f->releases;
    assert(d->submission && d->submission->buffers[0]->state == PS5VK_PENDING);
}
static uint64_t clock_ns(void *ctx) { return ((struct fixture *)ctx)->now; }
static void pause_wait(void *ctx, uint64_t remaining)
{ struct fixture *f = ctx; assert(remaining); ++f->now; f->complete = 1; }
static void record_empty(VkCommandBuffer c, VkCommandBufferUsageFlags flags)
{
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = flags};
    assert(vkBeginCommandBuffer(c, &info) == VK_SUCCESS);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);
}
static void recover_fixture(VkDevice d, struct fixture *f)
{
    /* Tests have no GPU or external ownership. This is teardown of a synthetic
     * fault, not a recovery API or permission to clear loss on a real device. */
    f->launch_result = f->poll_result = VK_SUCCESS; f->wrong = 0; f->complete = 1;
    d->lost = VK_FALSE; assert(ps5vk_queue_poll(d) == VK_SUCCESS);
}
int main(void)
{
    struct fixture f = {0};
    struct VkDevice_T d = {.progress = {&f, ps5vk_queue_poll, clock_ns, pause_wait},
        .submit_backend = {prepare, launch, poll_backend, release}};
    d.queue.device = &d; d.queue.next_serial = 1;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT}; VkCommandPool pool;
    assert(vkCreateCommandPool(&d, &pi, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}; VkCommandBuffer c;
    assert(vkAllocateCommandBuffers(&d, &ai, &c) == VK_SUCCESS);
    record_empty(c, 0);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
    assert(vkCreateFence(&d, &fi, NULL, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c};
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == VK_SUCCESS);
    assert(c->state == PS5VK_PENDING && fence->pending_serial == 1 && !fence->signaled);
    assert(vkWaitForFences(&d, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT);
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) == VK_ERROR_UNKNOWN && f.launches == 1);
    assert(vkResetCommandBuffer(c, 0) != VK_SUCCESS);
    f.complete = 1; assert(vkGetFenceStatus(&d, fence) == VK_SUCCESS);
    assert(!d.submission && c->state == PS5VK_EXECUTABLE && f.releases == 1 && d.queue.completed_serial == 1);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == VK_SUCCESS);
    assert(vkDeviceWaitIdle(&d) == VK_SUCCESS && f.releases == 2 && fence->signaled);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 0, NULL, fence) == VK_SUCCESS && fence->signaled && f.launches == 2);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    VkCommandBuffer duplicate[] = {c, c}; submit.commandBufferCount = 2; submit.pCommandBuffers = duplicate;
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == VK_ERROR_UNKNOWN && f.prepares == 2);
    record_empty(c, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == VK_SUCCESS);
    assert(d.submission->count == 2 && c->state == PS5VK_PENDING);
    assert(vkResetCommandBuffer(c, 0) != VK_SUCCESS);
    /* Re-submit while pending: backend must retire the old batch first. */
    unsigned old_releases = f.releases;
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) == VK_SUCCESS);
    assert(f.releases == old_releases + 1 && fence->signaled);
    assert(c->state == PS5VK_PENDING);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(c->state == PS5VK_EXECUTABLE);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    VkCommandBufferBeginInfo conflicting = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    assert(vkBeginCommandBuffer(c, &conflicting) != VK_SUCCESS);
    submit.commandBufferCount = 1; submit.pCommandBuffers = &c;
    f.prepare_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint64_t next = d.queue.next_serial;
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == f.prepare_result);
    assert(!d.submission && !fence->pending_serial && c->state == PS5VK_EXECUTABLE && d.queue.next_serial == next);
    f.prepare_result = VK_SUCCESS; record_empty(c, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == VK_SUCCESS);
    f.complete = f.wrong = 1; unsigned released = f.releases;
    assert(vkGetFenceStatus(&d, fence) == VK_ERROR_DEVICE_LOST);
    assert(d.lost && d.submission && c->state == PS5VK_PENDING && !fence->signaled && f.releases == released);
    assert(vkQueueWaitIdle(&d.queue) == VK_ERROR_DEVICE_LOST);
    recover_fixture(&d, &f); assert(c->state == PS5VK_INVALID);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS); record_empty(c, 0);
    f.launch_result = VK_ERROR_DEVICE_LOST;
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == VK_ERROR_DEVICE_LOST);
    assert(d.submission && c->state == PS5VK_PENDING && fence->pending_serial && !fence->signaled);
    recover_fixture(&d, &f);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS); record_empty(c, 0);
    f.launch_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == VK_ERROR_DEVICE_LOST);
    assert(d.lost && d.submission && fence->pending_serial && !fence->signaled);
    recover_fixture(&d, &f);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS); record_empty(c, 0);
    assert(vkQueueSubmit(&d.queue, 1, &submit, fence) == VK_SUCCESS);
    f.poll_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(vkGetFenceStatus(&d, fence) == VK_ERROR_DEVICE_LOST);
    assert(d.lost && d.submission && fence->pending_serial && !fence->signaled);
    recover_fixture(&d, &f);
    /* Graphics ownership fixture: real image binding, synthetic render objects
     * and control backend only. This never produces or executes GPU commands. */
    d.graphics_enabled = VK_TRUE; d.image_requirements = image_requirements;
    d.memory.allocate = memory_allocate; d.memory.release = memory_release;
    d.memory.flush = memory_sync; d.memory.invalidate = memory_sync;
    d.max_allocation = 4096; d.noncoherent_atom = 64;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {4,4,1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImage image; VkDeviceMemory memory;
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 256};
    assert(vkCreateImage(&d, &ii, NULL, &image) == VK_SUCCESS);
    assert(vkAllocateMemory(&d, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(&d, image, memory, 0) == VK_SUCCESS);
    struct VkImageView_T view = {.device = &d, .image = image};
    struct VkRenderPass_T pass = {.device = &d, .attachment_count = 1,
        .depth = {.attachment = VK_ATTACHMENT_UNUSED},
        .attachments = {{.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT}}};
    struct VkFramebuffer_T fb = {.device = &d, .width = 4, .height = 4, .attachment_count = 1,
        .attachments = {&view}, .formats = {VK_FORMAT_B8G8R8A8_UNORM}, .samples = {VK_SAMPLE_COUNT_1_BIT},
        .depth_attachment = VK_ATTACHMENT_UNUSED};
    struct VkPipeline_T pipeline = {.device = &d, .graphics = VK_TRUE, .graphics_state = &f,
        .color_format = VK_FORMAT_B8G8R8A8_UNORM};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = &pass, .framebuffer = &fb, .renderArea = {.extent = {4,4}}};
    assert(vkBeginCommandBuffer(c, &begin) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &pipeline);
    vkCmdDraw(c, 3, 1, 0, 0); vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRenderPass(c); assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    unsigned before = f.prepares;
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) != VK_SUCCESS && f.prepares == before);
    d.graphics_submit_enabled = VK_TRUE;
    image->display_busy = VK_TRUE;
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) != VK_SUCCESS && f.prepares == before);
    assert(!d.submission && !image->pending);
    image->display_busy = VK_FALSE;
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) == VK_SUCCESS);
    assert(pass.pending == 1 && fb.pending == 1 && view.pending == 1 && image->pending == 1 && pipeline.pending == 2);
    vkFreeMemory(&d, memory, NULL); assert(d.memories == memory);
    f.complete = f.wrong = 1;
    assert(ps5vk_queue_poll(&d) == VK_ERROR_DEVICE_LOST);
    assert(image->pending == 1 && pipeline.pending == 2);
    recover_fixture(&d, &f);
    assert(!pass.pending && !fb.pending && !view.pending && !image->pending && !pipeline.pending);
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS && !image->pending);
    vkFreeMemory(&d, memory, NULL);
    assert(!d.memories && c->state == PS5VK_INVALID);
    vkDestroyImage(&d, image, NULL);
    vkDestroyFence(&d, fence, NULL); vkDestroyCommandPool(&d, pool, NULL);
    assert(!d.fences && !d.command_pools && !d.submission);
    puts("Queue submit/retirement: pass (control backend only, no GPU work)");
}

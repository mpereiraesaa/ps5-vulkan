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
    unsigned event_ops_seen;
    unsigned fail_prepare_at;
    VkResult prepare_result, launch_result, poll_result;
    uint64_t serial, now;
    int complete, wrong;
};
struct mock_job { struct fixture *fixture; uint64_t serial; };
static VkResult prepare(VkDevice d, const struct ps5vk_submission *s, void **job)
{
    struct fixture *f = d->progress.context; ++f->prepares;
    assert(s->buffers[0]->state == (d->submission == s ? PS5VK_PENDING : PS5VK_EXECUTABLE));
    for (uint32_t b = 0; b < s->count; ++b) {
        uint32_t first = ps5vk_submission_first_operation(s, b);
        uint32_t count = ps5vk_submission_operation_count(s, b);
        assert(first <= s->buffers[b]->operation_count &&
            count <= s->buffers[b]->operation_count - first);
        for (uint32_t k = first; k < first + count; ++k)
            f->event_ops_seen += s->buffers[b]->operations[k].type >= PS5VK_EVENT_SET;
    }
    if (f->fail_prepare_at && f->prepares == f->fail_prepare_at)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (f->prepare_result) return f->prepare_result;
    struct mock_job *prepared = malloc(sizeof(*prepared));
    if (!prepared) return VK_ERROR_OUT_OF_HOST_MEMORY;
    prepared->fixture = f; prepared->serial = s->serial;
    f->serial = s->serial; *job = prepared; return VK_SUCCESS;
}
static VkResult launch(VkDevice d, void *job)
{
    struct fixture *f = ((struct mock_job *)job)->fixture;
    ++f->launches; f->complete = 0;
    assert(d->submission && d->submission->buffers[0]->state == PS5VK_PENDING);
    return f->launch_result;
}
static VkResult poll_backend(VkDevice d, void *job, uint64_t *completed)
{
    (void)d; struct mock_job *prepared = job;
    struct fixture *f = prepared->fixture; ++f->polls;
    *completed = f->complete ? prepared->serial + !!f->wrong : 0;
    return f->poll_result;
}
static void release(VkDevice d, void *job)
{
    struct mock_job *prepared = job;
    struct fixture *f = prepared->fixture; ++f->releases;
    if (d->submission)
        assert(d->submission->buffers[0]->state == PS5VK_PENDING);
    free(prepared);
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
    /* The queue does not advertise sparse binding. Reject before dereferencing
     * batches or mutating serial/submission/fence state, including count zero. */
    uint64_t sparse_serial = d.queue.next_serial;
    assert(vkQueueBindSparse(&d.queue, 0, NULL, fence) == VK_ERROR_VALIDATION_FAILED);
    assert(d.queue.next_serial == sparse_serial && !d.submission &&
        !fence->signaled && !fence->pending_serial);
    assert(vkQueueBindSparse(&d.queue, 1,
        (const VkBindSparseInfo *)(uintptr_t)1, fence) == VK_ERROR_VALIDATION_FAILED);
    assert(d.queue.next_serial == sparse_serial && !d.submission &&
        !fence->signaled && !fence->pending_serial);
    d.lost = VK_TRUE;
    assert(vkQueueBindSparse(&d.queue, 0, NULL, fence) == VK_ERROR_DEVICE_LOST);
    d.lost = VK_FALSE;
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

    /* Every native job is prepared transactionally before the first launch.
     * A later prepare failure releases earlier jobs and publishes no state. */
    record_empty(c, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    VkSubmitInfo prepare_chain[] = {
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
         .pCommandBuffers = &c},
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
         .pCommandBuffers = &c},
    };
    uint64_t prepare_serial = d.queue.next_serial;
    unsigned prepare_releases = f.releases;
    f.fail_prepare_at = f.prepares + 2;
    assert(vkQueueSubmit(&d.queue, 2, prepare_chain, NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(!d.submission && d.queue.next_serial == prepare_serial &&
        c->state == PS5VK_EXECUTABLE && !c->pending_count &&
        f.releases == prepare_releases + 1);
    f.fail_prepare_at = 0;

    /* Binary semaphore transitions are ordered by submit record. A signal is
     * visible only at retirement and a wait consumes it exactly once. */
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore;
    assert(vkCreateSemaphore(&d, &si, NULL, &semaphore) == VK_SUCCESS);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo signal_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &c,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &semaphore};
    assert(vkQueueSubmit(&d.queue, 1, &signal_submit, fence) == VK_SUCCESS);
    assert(!semaphore->signaled && semaphore->pending == 1 && !fence->signaled);
    unsigned lifetime_before = d.lifetime_errors;
    vkDestroySemaphore(&d, semaphore, NULL);
    assert(d.semaphores == semaphore && d.lifetime_errors == lifetime_before + 1);
    f.complete = 1;
    assert(vkGetFenceStatus(&d, fence) == VK_SUCCESS);
    assert(semaphore->signaled && !semaphore->pending);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo wait_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore,
        .pWaitDstStageMask = &wait_stage};
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &wait_submit, fence) == VK_SUCCESS);
    assert(!semaphore->signaled && !semaphore->pending && fence->signaled);

    VkSubmitInfo ordered[] = {
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1,
         .pSignalSemaphores = &semaphore},
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
         .pWaitSemaphores = &semaphore, .pWaitDstStageMask = &wait_stage},
    };
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 2, ordered, fence) == VK_SUCCESS);
    assert(!semaphore->signaled && !semaphore->pending && fence->signaled);

    /* The same dependency also orders two real backend jobs. Signal becomes
     * available at retirement of record 0, is consumed exactly once before
     * launching record 1, and the fence belongs only to the latter. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    VkCommandBufferBeginInfo semaphore_begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };
    assert(vkBeginCommandBuffer(c, &semaphore_begin) == VK_SUCCESS);
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    ordered[0].commandBufferCount = ordered[1].commandBufferCount = 1;
    ordered[0].pCommandBuffers = ordered[1].pCommandBuffers = &c;
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    unsigned semaphore_launches = f.launches;
    assert(vkQueueSubmit(&d.queue, 2, ordered, fence) == VK_SUCCESS);
    assert(f.launches == semaphore_launches + 1 && c->pending_count == 2 &&
        semaphore->pending == 2 && !semaphore->signaled && !fence->signaled);
    f.complete = 1;
    assert(ps5vk_queue_poll(&d) == VK_SUCCESS);
    assert(f.launches == semaphore_launches + 2 && c->pending_count == 1 &&
        semaphore->pending == 1 && !semaphore->signaled && !fence->signaled);
    f.complete = 1;
    assert(ps5vk_queue_poll(&d) == VK_SUCCESS);
    assert(!d.submission && !c->pending_count && !semaphore->pending &&
        !semaphore->signaled && fence->signaled);
    uint64_t unchanged_serial = d.queue.next_serial;
    assert(vkQueueSubmit(&d.queue, 1, &wait_submit, NULL) != VK_SUCCESS);
    assert(d.queue.next_serial == unchanged_serial && !semaphore->signaled);
    vkDestroySemaphore(&d, semaphore, NULL); assert(!d.semaphores);

    /* Device event transitions remain frontend operations. They execute in
     * command-buffer order and the wait dependency is not sent to AGC. */
    VkEventCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent event;
    assert(vkCreateEvent(&d, &ei, NULL, &event) == VK_SUCCESS);
    VkCommandBufferBeginInfo event_begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkCommandBuffer c2;
    assert(vkAllocateCommandBuffers(&d, &ai, &c2) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &event_begin) == VK_SUCCESS);
    vkCmdSetEvent(c, event, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c2, &event_begin) == VK_SUCCESS);
    vkCmdWaitEvents(c2, 1, &event, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, NULL, 0, NULL, 0, NULL);
    assert(vkEndCommandBuffer(c2) == VK_SUCCESS);
    VkCommandBuffer event_buffers[] = {c, c2};
    VkSubmitInfo event_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 2, .pCommandBuffers = event_buffers};
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &event_submit, fence) == VK_SUCCESS);
    assert(!fence->signaled && vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(fence->signaled && vkGetEventStatus(&d, event) == VK_EVENT_SET &&
        !event->pending && c->state == PS5VK_EXECUTABLE && c2->state == PS5VK_EXECUTABLE);
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &event_begin) == VK_SUCCESS);
    vkCmdResetEvent(c, event, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    event_submit.commandBufferCount = 1; event_submit.pCommandBuffers = &c;
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &event_submit, fence) == VK_SUCCESS);
    assert(vkGetEventStatus(&d, event) == VK_EVENT_RESET);

    assert(vkResetCommandBuffer(c2, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c2, &event_begin) == VK_SUCCESS);
    vkCmdWaitEvents(c2, 1, &event, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, NULL, 0, NULL, 0, NULL);
    assert(vkEndCommandBuffer(c2) == VK_SUCCESS);
    event_submit.pCommandBuffers = &c2;
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &event_submit, fence) == VK_SUCCESS);
    assert(d.submission && d.submission->frontend_only && !fence->signaled &&
        event->pending && event->pending_waits);
    /* Host-set after submission is invalid while a pending CB waits on the
     * event (VUID-vkSetEvent-event-09543).  Complete this synthetic invalid
     * submission by changing fixture state directly, not through the API. */
    assert(vkSetEvent(&d, event) == VK_ERROR_UNKNOWN);
    event->host_signaled = VK_TRUE;
    assert(ps5vk_queue_poll(&d) == VK_SUCCESS && d.submission && !d.submission->frontend_only);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS && fence->signaled);

    /* The valid host path signals before submit; WAIT consumes no event state
     * and the following dependency segment is submitted to the backend. */
    assert(vkResetEvent(&d, event) == VK_SUCCESS);
    assert(vkSetEvent(&d, event) == VK_SUCCESS);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &event_submit, fence) == VK_SUCCESS);
    assert(d.submission && !d.submission->frontend_only &&
        event->pending && event->pending_waits && !fence->signaled);
    assert(vkSetEvent(&d, event) == VK_ERROR_UNKNOWN);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS && fence->signaled &&
        !event->pending && !event->pending_waits);

    /* A mixed GPU -> SET -> GPU command prepares only the leading GPU segment
     * eagerly.  The trailing segment must be prepared at queue head, after the
     * frontend event effect, so backend preparation cannot snapshot stale
     * layout or memory state. */
    assert(vkResetEvent(&d, event) == VK_SUCCESS);
    assert(vkResetCommandBuffer(c2, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c2, &event_begin) == VK_SUCCESS);
    vkCmdPipelineBarrier(c2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
    vkCmdSetEvent(c2, event, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    vkCmdPipelineBarrier(c2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
    assert(vkEndCommandBuffer(c2) == VK_SUCCESS && c2->operation_count == 3);
    VkSemaphore segment_semaphore;
    assert(vkCreateSemaphore(&d, &si, NULL, &segment_semaphore) == VK_SUCCESS);
    event_submit.signalSemaphoreCount = 1;
    event_submit.pSignalSemaphores = &segment_semaphore;
    unsigned mixed_launches = f.launches;
    f.fail_prepare_at = f.prepares + 2;
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &event_submit, fence) == VK_SUCCESS);
    assert(f.launches == mixed_launches + 1 && d.submission &&
        d.submission->next && d.submission->next->frontend_only &&
        d.submission->next->next && d.submission->next->next->deferred_prepare &&
        c2->state == PS5VK_PENDING && c2->pending_count == 3 &&
        vkGetEventStatus(&d, event) == VK_EVENT_RESET && event->pending &&
        segment_semaphore->pending && !segment_semaphore->signaled &&
        fence->pending_serial && !fence->signaled && !f.event_ops_seen);
    f.complete = 1;
    assert(ps5vk_queue_poll(&d) == VK_ERROR_DEVICE_LOST);
    assert(d.lost && f.launches == mixed_launches + 1 && d.submission &&
        !d.submission->frontend_only && d.submission->deferred_prepare &&
        event->device_signaled && event->pending &&
        !segment_semaphore->signaled && !fence->signaled);
    /* A real device remains lost after a deferred-prepare failure.  The host
     * fixture has no recovery API, so install and launch a replacement mock
     * job explicitly only to release the synthetic submission graph. */
    f.fail_prepare_at = 0; d.lost = VK_FALSE;
    assert(prepare(&d, d.submission, &d.submission->backend_job) == VK_SUCCESS);
    assert(d.submission->backend_job && launch(&d, d.submission->backend_job) == VK_SUCCESS);
    f.complete = 1;
    assert(ps5vk_queue_poll(&d) == VK_SUCCESS && !d.submission);
    assert(segment_semaphore->signaled && fence->signaled);

    assert(vkResetEvent(&d, event) == VK_SUCCESS);
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    segment_semaphore->signaled = VK_FALSE;
    mixed_launches = f.launches;
    assert(vkQueueSubmit(&d.queue, 1, &event_submit, fence) == VK_SUCCESS);
    assert(f.launches == mixed_launches + 1 && d.submission &&
        d.submission->next && d.submission->next->frontend_only &&
        d.submission->next->next && !d.submission->next->next->frontend_only &&
        c2->pending_count == 3 && vkGetEventStatus(&d, event) == VK_EVENT_RESET &&
        event->pending && !event->pending_waits &&
        !segment_semaphore->signaled && !fence->signaled && !f.event_ops_seen);
    f.complete = 1;
    assert(ps5vk_queue_poll(&d) == VK_SUCCESS);
    assert(f.launches == mixed_launches + 2 && d.submission &&
        !d.submission->frontend_only && c2->pending_count == 1 &&
        vkGetEventStatus(&d, event) == VK_EVENT_SET &&
        event->pending && !event->pending_waits &&
        !segment_semaphore->signaled && !fence->signaled && !f.event_ops_seen);
    f.complete = 1;
    assert(ps5vk_queue_poll(&d) == VK_SUCCESS);
    assert(!d.submission && !c2->pending_count && c2->state == PS5VK_EXECUTABLE &&
        !event->pending && !event->pending_waits &&
        segment_semaphore->signaled && !segment_semaphore->pending &&
        fence->signaled && !f.event_ops_seen);
    vkDestroySemaphore(&d, segment_semaphore, NULL);
    vkDestroyEvent(&d, event, NULL); assert(!d.events);
    vkFreeCommandBuffers(&d, pool, 1, &c2);

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
    VkAttachmentDescription pass_attachments[1] = {
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT}};
    struct ps5vk_subpass pass_subpasses[1] = {
        {.color[0] = {.attachment = 0}, .color_count = 1, .depth = {.attachment = VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T pass = {.device = &d, .attachment_count = 1, .subpass_count = 1,
        .attachments = pass_attachments, .subpasses = pass_subpasses};
    struct VkFramebuffer_T fb = {.device = &d, .width = 4, .height = 4, .attachment_count = 1, .color_attachments = {0}, .color_count = 1,
        .attachments = {&view}, .formats = {VK_FORMAT_B8G8R8A8_UNORM}, .samples = {VK_SAMPLE_COUNT_1_BIT},
        .depth_attachment = VK_ATTACHMENT_UNUSED};
    struct VkPipeline_T pipeline = {.device = &d, .graphics = VK_TRUE, .viewport_count = 1, .graphics_state = &f,
        .color_format = {VK_FORMAT_B8G8R8A8_UNORM}, .color_attachment_count = 1};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = &pass, .framebuffer = &fb, .renderArea = {.extent = {4,4}}};
    assert(vkBeginCommandBuffer(c, &begin) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &pipeline);
    struct VkDescriptorPool_T sampled_pool={.device=&d};
    struct VkDescriptorSet_T sampled_sets[4]={0};VkDescriptorSet sampled_handles[4];
    struct VkPipelineLayout_T sampled_layout={.device=&d,.set_count=4};
    pipeline.set_count=4;
    for(unsigned s=0;s<4;++s) {
        sampled_sets[s].pool=&sampled_pool;sampled_sets[s].generation=8+s;
        sampled_sets[s].signature.count=1;
        sampled_sets[s].signature.binding[7]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_FRAGMENT_BIT};
        sampled_sets[s].signature.type[7]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        for(unsigned b=8;b<PS5VK_MAX_BINDINGS;++b)sampled_sets[s].signature.binding[b].first=1;
        sampled_layout.sets[s]=pipeline.sets[s]=sampled_sets[s].signature;
        sampled_handles[s]=sampled_sets+s;
    }
    vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&sampled_layout,0,4,sampled_handles,0,NULL);
    vkCmdDraw(c, 3, 1, 0, 0); vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRenderPass(c); assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    unsigned before = f.prepares;
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) != VK_SUCCESS && f.prepares == before);
    d.graphics_submit_enabled = VK_TRUE;
    image->display_busy = VK_TRUE;
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) != VK_SUCCESS && f.prepares == before);
    assert(!d.submission && !image->pending);
    image->display_busy = VK_FALSE;
    sampled_sets[3].generation++;
    assert(vkQueueSubmit(&d.queue,1,&submit,NULL)!=VK_SUCCESS && f.prepares==before && !d.submission);
    for(unsigned s=0;s<4;++s)assert(!sampled_sets[s].pending);
    sampled_sets[3].generation--;
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) == VK_SUCCESS);
    for(unsigned s=0;s<4;++s)assert(sampled_sets[s].pending==2);
    assert(pass.pending == 1 && fb.pending == 1 && view.pending == 1 && image->pending == 1 && pipeline.pending == 2);
    vkFreeMemory(&d, memory, NULL); assert(d.memories == memory);
    f.complete = f.wrong = 1;
    assert(ps5vk_queue_poll(&d) == VK_ERROR_DEVICE_LOST);
    assert(image->pending == 1 && pipeline.pending == 2);
    for(unsigned s=0;s<4;++s)assert(sampled_sets[s].pending==2);
    recover_fixture(&d, &f);
    assert(!pass.pending && !fb.pending && !view.pending && !image->pending && !pipeline.pending);
    for(unsigned s=0;s<4;++s)assert(!sampled_sets[s].pending);
    assert(vkQueueSubmit(&d.queue, 1, &submit, NULL) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS && !image->pending);

    /* The layout remains compatible, but an executable using no descriptors
     * needs none bound. Unknown or actually-used sets are still mandatory. */
    for(unsigned s=0;s<4;++s) {
        c->operations[1].sets[s]=c->operations[2].sets[s]=NULL;
    }
    pipeline.graphics_usage_known=VK_TRUE;pipeline.graphics_used_set_mask=0;
    assert(vkQueueSubmit(&d.queue,1,&submit,NULL)==VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue)==VK_SUCCESS);
    for(unsigned s=0;s<4;++s)assert(!sampled_sets[s].pending);
    pipeline.graphics_used_set_mask=1;
    assert(vkQueueSubmit(&d.queue,1,&submit,NULL)!=VK_SUCCESS && !d.submission);
    pipeline.graphics_usage_known=VK_FALSE;
    assert(vkQueueSubmit(&d.queue,1,&submit,NULL)!=VK_SUCCESS && !d.submission);
    pipeline.graphics_used_set_mask=0;
    for(unsigned s=0;s<4;++s)c->operations[1].sets[s]=c->operations[2].sets[s]=&sampled_sets[s];

    /* The bounded two-subpass shared-role shape reaches the backend. The
     * immutable stream must retain both draws and the exact transition. */
    {
        struct ps5vk_subpass two_subpasses[2] = {pass_subpasses[0], pass_subpasses[0]};
        struct VkRenderPass_T two = {.device = &d, .attachment_count = 1, .subpass_count = 2,
            .attachments = pass_attachments, .subpasses = two_subpasses};
        VkCommandBuffer multi;
        VkCommandBufferAllocateInfo mi2 = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        assert(vkAllocateCommandBuffers(&d, &mi2, &multi) == VK_SUCCESS);
        VkRenderPassBeginInfo two_ri = ri;
        two_ri.renderPass = &two;
        assert(vkBeginCommandBuffer(multi, &begin) == VK_SUCCESS);
        /* One pipeline per subpass: a pipeline created for subpass 0 may not
         * draw in subpass 1, so the second draw needs its own identity. */
        struct VkPipeline_T second = pipeline; second.subpass = 1;
        vkCmdBeginRenderPass(multi, &two_ri, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(multi, VK_PIPELINE_BIND_POINT_GRAPHICS, &pipeline);
        vkCmdBindDescriptorSets(multi, VK_PIPELINE_BIND_POINT_GRAPHICS, &sampled_layout,
                                0, 4, sampled_handles, 0, NULL);
        vkCmdDraw(multi, 3, 1, 0, 0);
        vkCmdNextSubpass(multi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(multi, VK_PIPELINE_BIND_POINT_GRAPHICS, &second);
        vkCmdBindDescriptorSets(multi, VK_PIPELINE_BIND_POINT_GRAPHICS, &sampled_layout,
                                0, 4, sampled_handles, 0, NULL);
        vkCmdDraw(multi, 3, 1, 0, 0);
        vkCmdEndRenderPass(multi);
        /* Recording accepted the whole thing... */
        assert(vkEndCommandBuffer(multi) == VK_SUCCESS && multi->operation_count == 5);
        VkSubmitInfo multi_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &multi};
        const unsigned prepared = f.prepares;
        assert(vkQueueSubmit(&d.queue, 1, &multi_submit, NULL) == VK_SUCCESS);
        assert(f.prepares == prepared + 1 && d.submission && multi->pending_count == 1 &&
               two.pending == 1 && fb.pending == 1 && image->pending == 1 &&
               pipeline.pending == 1 && second.pending == 1);
        f.complete = 1;
        assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
        assert(!d.submission && !multi->pending_count && !two.pending &&
               !fb.pending && !image->pending && !pipeline.pending && !second.pending);

        /* Submission re-derives transition ordering rather than trusting the
         * recorder. A repeated/jumped marker in an otherwise executable
         * stream is rejected before prepare and leaves ownership untouched. */
        multi->operations[2].subpass = 0;
        const unsigned after_success = f.prepares;
        assert(vkQueueSubmit(&d.queue, 1, &multi_submit, NULL) != VK_SUCCESS);
        assert(f.prepares == after_success && !d.submission && !multi->pending_count);
        multi->operations[2].subpass = 1;
        vkFreeCommandBuffers(&d, pool, 1, &multi);
    }
    vkFreeMemory(&d, memory, NULL);
    assert(!d.memories && c->state == PS5VK_INVALID);
    vkDestroyImage(&d, image, NULL);
    vkDestroyFence(&d, fence, NULL); vkDestroyCommandPool(&d, pool, NULL);
    assert(!d.fences && !d.command_pools && !d.submission);
    puts("Queue submit/retirement: pass (control backend only, no GPU work)");
}

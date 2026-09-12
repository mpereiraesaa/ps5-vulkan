#include "vk_queue.h"

#define INVALID VK_ERROR_UNKNOWN
static void free_submission(struct ps5vk_submission *s)
{
    VkAllocationCallbacks a = s->allocator; VkBool32 custom = s->custom_allocator;
    ps5vk_object_free(s, &a, custom);
}
static void pin(struct ps5vk_submission *s, int acquire)
{
    for (unsigned j = 0; j < s->count; ++j) {
        VkCommandBuffer c = s->buffers[j];
        for (unsigned k = 0; k < c->operation_count; ++k) {
            struct ps5vk_operation *op = &c->operations[k];
            if (op->type == PS5VK_BEGIN_RENDER_PASS) {
                if (acquire) { ++op->render_pass->pending; ++op->framebuffer->pending; }
                else { --op->render_pass->pending; --op->framebuffer->pending; }
                for (uint32_t n = 0; n < op->framebuffer->attachment_count; ++n) {
                    VkImageView view = op->framebuffer->attachments[n];
                    if (acquire) { ++view->pending; ++view->image->pending; }
                    else { --view->pending; --view->image->pending; }
                }
            }
            if (op->type == PS5VK_DRAW || op->type == PS5VK_DRAW_INDEXED) {
                if (acquire) ++op->pipeline->pending;
                else --op->pipeline->pending;
                if(op->set) {if(acquire)++op->set->pending;else --op->set->pending;}
            }
            if (op->type != PS5VK_DISPATCH) continue;
            if (acquire) { ++op->pipeline->pending; ++op->set->pending; }
            else { --op->pipeline->pending; --op->set->pending; }
        }
        c->state = acquire ? PS5VK_PENDING :
            (c->usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT ? PS5VK_INVALID : PS5VK_EXECUTABLE);
    }
}
VkResult ps5vk_queue_poll(VkDevice d)
{
    if (!d) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    struct ps5vk_submission *s = d->submission;
    if (!s) return VK_SUCCESS;
    if (!d->submit_backend.poll || !d->submit_backend.release) { d->lost = VK_TRUE; return VK_ERROR_DEVICE_LOST; }
    uint64_t completed = 0;
    VkResult result = d->submit_backend.poll(d, s->backend_job, &completed);
    if (result != VK_SUCCESS) { d->lost = VK_TRUE; return VK_ERROR_DEVICE_LOST; }
    if (!completed) return VK_SUCCESS;
    if (completed != s->serial) { d->lost = VK_TRUE; return VK_ERROR_DEVICE_LOST; }
    /* The backend contract includes visibility and exact completion. Release
     * its private GPU allocations before reporting successful retirement. */
    d->submit_backend.release(d, s->backend_job);
    pin(s, 0);
    d->queue.completed_serial = s->serial;
    if (s->fence) { s->fence->pending_serial = 0; s->fence->signaled = VK_TRUE; }
    d->submission = NULL; free_submission(s);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
    if (!queue || !queue->device) return INVALID;
    VkDevice d = queue->device;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    while (d->submission) {
        VkResult result = ps5vk_queue_poll(d);
        if (result != VK_SUCCESS) return result;
        if (!d->submission) break;
        if (!d->progress.pause) return INVALID;
        d->progress.pause(d->progress.context, UINT64_MAX);
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice d)
{ return d ? vkQueueWaitIdle(&d->queue) : INVALID; }

static int command_valid(VkDevice d, VkCommandBuffer c)
{
    if (!c || c->pool->device != d || c->state != PS5VK_EXECUTABLE) return 0;
    VkRenderPass active = NULL;
    VkFramebuffer framebuffer = NULL;
    for (unsigned j = 0; j < c->operation_count; ++j) {
        const struct ps5vk_operation *op = &c->operations[j];
        if (op->type == PS5VK_BEGIN_RENDER_PASS || op->type == PS5VK_DRAW || op->type == PS5VK_DRAW_INDEXED || op->type == PS5VK_END_RENDER_PASS) {
            if (!d->graphics_enabled || !d->graphics_submit_enabled || !op->render_pass || !op->framebuffer ||
                op->render_pass->device != d || op->framebuffer->device != d) return 0;
            if (op->type == PS5VK_BEGIN_RENDER_PASS) {
                if (active) return 0;
                active = op->render_pass; framebuffer = op->framebuffer;
                for (uint32_t n = 0; n < framebuffer->attachment_count; ++n) {
                    VkImageView view = framebuffer->attachments[n];
                    void *address; VkDeviceSize bytes;
                    if (!view || view->device != d || !view->image || view->image->display_busy ||
                        ps5vk_image_span(d, view->image, &address, &bytes) != VK_SUCCESS) return 0;
                }
            } else {
                if (active != op->render_pass || framebuffer != op->framebuffer) return 0;
                if (op->type == PS5VK_DRAW || op->type == PS5VK_DRAW_INDEXED) {
                    if (!op->pipeline || op->pipeline->device != d || !op->pipeline->graphics ||
                        !op->pipeline->graphics_state) return 0;
                    if(op->pipeline->set_count && (!op->set || op->set->pool->device!=d ||
                        op->generation!=op->set->generation))return 0;
                } else { active = NULL; framebuffer = NULL; }
            }
            continue;
        }
        if (active) return 0;
        if(op->type==PS5VK_IMAGE_BARRIER || op->type==PS5VK_COPY_BUFFER_IMAGE) {
            VkImage image=op->type==PS5VK_IMAGE_BARRIER?op->image_barrier.image:op->copy_image;
            void *address;VkDeviceSize bytes;
            if(!d->graphics_enabled || !d->graphics_submit_enabled || !image || image->display_busy ||
                ps5vk_image_span(d,image,&address,&bytes)!=VK_SUCCESS)return 0;
            continue;
        }
        if (op->type == PS5VK_BARRIER) continue;
        if (op->type != PS5VK_DISPATCH || !op->pipeline || op->pipeline->graphics || !op->set ||
            op->pipeline->device != d || op->set->pool->device != d || op->set->generation != op->generation) return 0;
        const struct ps5vk_compiled_program *p = &op->pipeline->program;
        for (uint32_t k = 0; k < p->descriptor_count; ++k) {
            const struct ps5vk_program_descriptor *b = &p->descriptors[k];
            unsigned index = op->set->signature.binding[b->binding].first + b->element;
            void *address; VkDeviceSize size;
            if (!op->set->defined[index] || ps5vk_buffer_span(d, op->set->buffers[index].buffer,
                op->set->buffers[index].offset, op->set->buffers[index].range, &address, &size) != VK_SUCCESS) return 0;
        }
    }
    return active == NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence)
{
    if (!queue || !queue->device || (count && !infos)) return INVALID;
    VkDevice d = queue->device;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    if (fence && (fence->device != d || fence->signaled || fence->pending_serial)) return INVALID;
    unsigned buffers = 0;
    for (uint32_t j = 0; j < count; ++j) {
        const VkSubmitInfo *info = &infos[j];
        if (info->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || info->pNext ||
            info->waitSemaphoreCount || info->signalSemaphoreCount ||
            (info->commandBufferCount && !info->pCommandBuffers) ||
            info->commandBufferCount > PS5VK_MAX_SUBMITTED_BUFFERS - buffers) return INVALID;
        buffers += info->commandBufferCount;
        for (uint32_t k = 0; k < info->commandBufferCount; ++k)
            if (!command_valid(d, info->pCommandBuffers[k])) return INVALID;
    }
    if (buffers && (!d->submit_backend.prepare || !d->submit_backend.launch ||
                    !d->submit_backend.poll || !d->submit_backend.release)) return INVALID;
    if (!d->queue.next_serial || d->queue.next_serial == UINT64_MAX) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    struct ps5vk_submission *s = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, NULL,
        sizeof(*s), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &saved, &custom);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->allocator = saved; s->custom_allocator = custom; s->fence = fence;
    for (uint32_t j = 0; j < count; ++j) for (uint32_t k = 0; k < infos[j].commandBufferCount; ++k) {
        VkCommandBuffer c = infos[j].pCommandBuffers[k];
        for (unsigned n = 0; n < s->count; ++n) if (s->buffers[n] == c) { free_submission(s); return INVALID; }
        s->buffers[s->count++] = c;
    }
    /* One backend batch in flight. QueueSubmit may block to preserve ordering;
     * there is no simultaneous-use or semaphore support in this profile. */
    VkResult result = vkQueueWaitIdle(queue);
    if (result != VK_SUCCESS) { free_submission(s); return result; }
    s->serial = d->queue.next_serial;
    if (!buffers) {
        /* No commands and all previous work retired: an empty fence submission
         * requires no fabricated GPU job or callback success. */
        ++d->queue.next_serial; d->queue.completed_serial = s->serial;
        if (fence) fence->signaled = VK_TRUE;
        free_submission(s); return VK_SUCCESS;
    }
    result = d->submit_backend.prepare(d, s, &s->backend_job);
    if (result != VK_SUCCESS) { free_submission(s); return result < 0 ? result : INVALID; }
    if (!s->backend_job) { free_submission(s); return INVALID; }
    pin(s, 1); d->submission = s; ++d->queue.next_serial;
    if (fence) fence->pending_serial = s->serial;
    d->progress.poll = ps5vk_queue_poll;
    result = d->submit_backend.launch(d, s->backend_job);
    if (result != VK_SUCCESS) {
        /* After launch is attempted ownership is ambiguous. No unpin/free or
         * misleading fence signal: exact-title closure may be needed. */
        d->lost = VK_TRUE; return VK_ERROR_DEVICE_LOST;
    }
    return VK_SUCCESS;
}

#include "vk_queue.h"

#define INVALID VK_ERROR_UNKNOWN
static void free_submission(struct ps5vk_submission *s)
{
    if (s->reserved) {
        for (uint32_t j = 0; j < s->wait_count; ++j) --s->waits[j]->pending;
        for (uint32_t j = 0; j < s->signal_count; ++j) --s->signals[j]->pending;
    }
    VkAllocationCallbacks a = s->allocator; VkBool32 custom = s->custom_allocator;
    ps5vk_object_free(s, &a, custom);
}
static void pin(struct ps5vk_submission *s, int acquire)
{
    for (unsigned j = 0; j < s->count; ++j) {
        VkCommandBuffer c = s->buffers[j];
        for (unsigned k = 0; k < c->operation_count; ++k) {
            struct ps5vk_operation *op = &c->operations[k];
            if (op->type == PS5VK_EVENT_SET || op->type == PS5VK_EVENT_RESET ||
                op->type == PS5VK_EVENT_WAIT) {
                if (acquire) ++op->event->pending; else --op->event->pending;
            }
            if (op->type == PS5VK_EVENT_SET || op->type == PS5VK_EVENT_RESET ||
                op->type == PS5VK_EVENT_WAIT) {
                if (acquire) ++op->event->pending; else --op->event->pending;
                continue;
            }
            if (op->type == PS5VK_EVENT_SET || op->type == PS5VK_EVENT_RESET ||
                op->type == PS5VK_EVENT_WAIT) {
                if (acquire) ++op->event->pending; else --op->event->pending;
            }
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
                if(op->sets[0]) {if(acquire)++op->sets[0]->pending;else --op->sets[0]->pending;}
            }
            if (op->type != PS5VK_DISPATCH) continue;
            if (acquire) ++op->pipeline->pending; else --op->pipeline->pending;
            for (uint32_t set=0;set<PS5VK_MAX_SETS;++set) if(op->sets[set]) {
                if(acquire)++op->sets[set]->pending;else --op->sets[set]->pending;
            }
        }
        if (acquire) {
            ++c->pending_count;
            c->state = PS5VK_PENDING;
        } else {
            --c->pending_count;
            if (!c->pending_count)
                c->state = c->usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT ?
                    PS5VK_INVALID : PS5VK_EXECUTABLE;
        }
    }
}

static VkResult start_submission(VkDevice d)
{
    while (d->submission) {
        struct ps5vk_submission *s = d->submission;
        for (uint32_t j = 0; j < s->wait_count; ++j) {
            if (!s->waits[j]->signaled) { d->lost = VK_TRUE; return VK_ERROR_DEVICE_LOST; }
            s->waits[j]->signaled = VK_FALSE;
        }
        if (s->frontend_only) {
            for (uint32_t b = 0; b < s->count; ++b) {
                VkCommandBuffer command = s->buffers[b];
                for (uint32_t k = 0; k < command->operation_count; ++k) {
                    struct ps5vk_operation *op = &command->operations[k];
                    if (op->type == PS5VK_EVENT_WAIT &&
                        !(op->event->host_signaled || op->event->device_signaled)) {
                        d->lost = VK_TRUE; return VK_ERROR_DEVICE_LOST;
                    }
                    if (op->type == PS5VK_EVENT_SET) op->event->device_signaled = VK_TRUE;
                    if (op->type == PS5VK_EVENT_RESET) {
                        op->event->host_signaled = VK_FALSE;
                        op->event->device_signaled = VK_FALSE;
                    }
                }
            }
            pin(s, 0);
        } else if (s->count) {
            VkResult result = d->submit_backend.launch(d, s->backend_job);
            if (result != VK_SUCCESS) { d->lost = VK_TRUE; return VK_ERROR_DEVICE_LOST; }
            return VK_SUCCESS;
        }
        d->queue.completed_serial = s->serial;
        for (uint32_t j = 0; j < s->signal_count; ++j) s->signals[j]->signaled = VK_TRUE;
        if (s->fence) { s->fence->pending_serial = 0; s->fence->signaled = VK_TRUE; }
        d->submission = s->next; free_submission(s);
    }
    return VK_SUCCESS;
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
    /* The backend contract includes visibility and exact completion. */
    d->submit_backend.release(d, s->backend_job);
    pin(s, 0);
    d->queue.completed_serial = s->serial;
    for (uint32_t j = 0; j < s->signal_count; ++j) s->signals[j]->signaled = VK_TRUE;
    if (s->fence) { s->fence->pending_serial = 0; s->fence->signaled = VK_TRUE; }
    d->submission = s->next; free_submission(s);

    return start_submission(d);
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
        if (op->type == PS5VK_EVENT_SET || op->type == PS5VK_EVENT_RESET ||
            op->type == PS5VK_EVENT_WAIT) {
            if (active || !op->event || op->event->device != d ||
                !op->src_stage || !op->dst_stage) return 0;
            continue;
        }
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
                    if(op->pipeline->set_count && (!op->sets[0] || op->sets[0]->pool->device!=d ||
                        op->generations[0]!=op->sets[0]->generation))return 0;
                } else { active = NULL; framebuffer = NULL; }
            }
            continue;
        }
        if (active) return 0;
        if (op->type == PS5VK_EVENT_SET || op->type == PS5VK_EVENT_RESET ||
            op->type == PS5VK_EVENT_WAIT) {
            if (!op->event || op->event->device != d) return 0;
            continue;
        }
        if(op->type==PS5VK_IMAGE_BARRIER || op->type==PS5VK_COPY_BUFFER_IMAGE ||
           op->type==PS5VK_COPY_IMAGE_BUFFER) {
            VkImage image=op->type==PS5VK_IMAGE_BARRIER?op->image_barrier.image:op->copy_image;
            void *address;VkDeviceSize bytes;
            if(!d->graphics_enabled || !d->graphics_submit_enabled || !image || image->display_busy ||
                ps5vk_image_span(d,image,&address,&bytes)!=VK_SUCCESS)return 0;
            if(op->type==PS5VK_COPY_IMAGE_BUFFER &&
               ps5vk_buffer_span(d,op->copy_destination,0,VK_WHOLE_SIZE,&address,&bytes)!=VK_SUCCESS)
                return 0;
            continue;
        }
        if (op->type == PS5VK_BARRIER) {
            if (op->buffer_barrier.buffer) {
                void *address; VkDeviceSize bytes;
                if (ps5vk_buffer_span(d, op->buffer_barrier.buffer, op->buffer_barrier.offset,
                    op->buffer_barrier.size, &address, &bytes) != VK_SUCCESS) return 0;
            }
            continue;
        }
        if (op->type == PS5VK_EVENT_SET || op->type == PS5VK_EVENT_RESET ||
            op->type == PS5VK_EVENT_WAIT) {
            if (!op->event || op->event->device != d) return 0;
            continue;
        }
        if (op->type != PS5VK_DISPATCH || !op->pipeline || op->pipeline->graphics ||
            op->pipeline->device != d) return 0;
        const struct ps5vk_compiled_program *p = &op->pipeline->program;
        for(uint32_t set=0;set<PS5VK_MAX_SETS;++set) if(p->descriptor_set_mask&(1u<<set)) {
            if(!op->sets[set] || op->sets[set]->pool->device!=d ||
               op->sets[set]->generation!=op->generations[set])return 0;
        }
        for (uint32_t k = 0; k < p->descriptor_count; ++k) {
            const struct ps5vk_program_descriptor *b = &p->descriptors[k];
            VkDescriptorSet set=op->sets[b->set];
            unsigned index = set->signature.binding[b->binding].first + b->element;
            if (!set->defined[index]) return 0;
        }
    }
    return active == NULL;
}
struct semaphore_state { VkSemaphore semaphore; VkBool32 signaled; };

static void discard_chain(VkDevice d, struct ps5vk_submission *head)
{
    while (head) {
        struct ps5vk_submission *next = head->next;
        if (head->backend_job && d->submit_backend.release)
            d->submit_backend.release(d, head->backend_job);
        free_submission(head); head = next;
    }
}

static struct semaphore_state *find_state(struct semaphore_state *states,
    uint32_t *count, VkSemaphore semaphore)
{
    for (uint32_t j = 0; j < *count; ++j)
        if (states[j].semaphore == semaphore) return &states[j];
    states[*count].semaphore = semaphore;
    states[*count].signaled = semaphore->signaled;
    return &states[(*count)++];
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count,
    const VkSubmitInfo *infos, VkFence fence)
{
    if (!queue || !queue->device || (count && !infos)) return INVALID;
    VkDevice d = queue->device;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    if (fence && (fence->device != d || fence->signaled || fence->pending_serial)) return INVALID;

    uint32_t records = count ? count : 1, reference_count = 0;
    for (uint32_t j = 0; j < count; ++j) {
        const VkSubmitInfo *info = &infos[j];
        if (info->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || info->pNext ||
            (info->waitSemaphoreCount && (!info->pWaitSemaphores || !info->pWaitDstStageMask)) ||
            (info->signalSemaphoreCount && !info->pSignalSemaphores) ||
            (info->commandBufferCount && !info->pCommandBuffers) ||
            info->commandBufferCount > PS5VK_MAX_SUBMITTED_BUFFERS ||
            UINT32_MAX - reference_count < info->waitSemaphoreCount ||
            UINT32_MAX - reference_count - info->waitSemaphoreCount < info->signalSemaphoreCount)
            return INVALID;
        reference_count += info->waitSemaphoreCount + info->signalSemaphoreCount;
        for (uint32_t k = 0; k < info->waitSemaphoreCount; ++k)
            if (!info->pWaitDstStageMask[k]) return INVALID;
        for (uint32_t k = 0; k < info->commandBufferCount; ++k) {
            VkCommandBuffer command = info->pCommandBuffers[k];
            if (command && command->pool && command->pool->device == d &&
                command->state == PS5VK_PENDING &&
                !(command->usage & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT))
                return INVALID;
        }
    }
    if (!d->queue.next_serial || records > UINT64_MAX - d->queue.next_serial) return INVALID;

    /* This implementation has one native batch in flight. Retire the previous
     * call before validating this call, preserving queue order. */
    VkResult result = vkQueueWaitIdle(queue);
    if (result != VK_SUCCESS) return result;

    size_t state_bytes = (size_t)reference_count * sizeof(struct semaphore_state);
    if (reference_count && state_bytes / sizeof(struct semaphore_state) != reference_count)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkAllocationCallbacks state_allocator = {0}; VkBool32 state_custom = VK_FALSE;
    struct semaphore_state *states = reference_count ? ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, NULL, state_bytes,
        VK_SYSTEM_ALLOCATION_SCOPE_COMMAND, &state_allocator, &state_custom) : NULL;
    if (reference_count && !states) return VK_ERROR_OUT_OF_HOST_MEMORY;

    struct ps5vk_submission *head = NULL, **tail = &head;
    uint32_t state_count = 0;
    for (uint32_t j = 0; j < records; ++j) {
        const VkSubmitInfo *info = count ? &infos[j] : NULL;
        uint32_t waits = info ? info->waitSemaphoreCount : 0;
        uint32_t signals = info ? info->signalSemaphoreCount : 0;
        size_t refs = (size_t)waits + signals;
        if (refs > (SIZE_MAX - sizeof(struct ps5vk_submission)) / sizeof(VkSemaphore)) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail;
        }
        VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
        struct ps5vk_submission *s = ps5vk_object_alloc(
            d->custom_allocator ? &d->allocator : NULL, NULL,
            sizeof(*s) + refs * sizeof(VkSemaphore),
            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &saved, &custom);
        if (!s) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail; }
        s->allocator = saved; s->custom_allocator = custom;
        s->serial = d->queue.next_serial + j;
        s->wait_count = waits; s->signal_count = signals;
        s->waits = (VkSemaphore *)(s + 1); s->signals = s->waits + waits;
        *tail = s; tail = &s->next;
        if (!info) continue;

        for (uint32_t k = 0; k < waits; ++k) {
            VkSemaphore semaphore = info->pWaitSemaphores[k];
            if (!semaphore || semaphore->device != d) { result = INVALID; goto fail; }
            struct semaphore_state *state = find_state(states, &state_count, semaphore);
            if (!state->signaled) { result = INVALID; goto fail; }
            state->signaled = VK_FALSE; s->waits[k] = semaphore;
        }
        for (uint32_t k = 0; k < signals; ++k) {
            VkSemaphore semaphore = info->pSignalSemaphores[k];
            if (!semaphore || semaphore->device != d) { result = INVALID; goto fail; }
            struct semaphore_state *state = find_state(states, &state_count, semaphore);
            if (state->signaled) { result = INVALID; goto fail; }
            state->signaled = VK_TRUE; s->signals[k] = semaphore;
        }
        for (uint32_t k = 0; k < info->commandBufferCount; ++k) {
            VkCommandBuffer command = info->pCommandBuffers[k];
            if (!command_valid(d, command)) { result = INVALID; goto fail; }
            for (struct ps5vk_submission *prior = head; prior; prior = prior->next)
                for (uint32_t n = 0; n < prior->count; ++n)
                    if (prior->buffers[n] == command &&
                        !(command->usage & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) {
                        result = INVALID; goto fail;
                    }
            s->buffers[s->count++] = command;
        }
        VkBool32 has_event = VK_FALSE, has_gpu = VK_FALSE;
        for (uint32_t k = 0; k < s->count; ++k)
            for (uint32_t n = 0; n < s->buffers[k]->operation_count; ++n) {
                int type = s->buffers[k]->operations[n].type;
                if (type == PS5VK_EVENT_SET || type == PS5VK_EVENT_RESET ||
                    type == PS5VK_EVENT_WAIT) has_event = VK_TRUE;
                else if (type != PS5VK_BARRIER) has_gpu = VK_TRUE;
            }
        /* Mixed buffers require range segmentation; never pass event opcodes
         * to a native backend that cannot interpret them. */
        if (has_event && has_gpu) { result = INVALID; goto fail; }
        s->frontend_only = has_event;
        if (s->count && !s->frontend_only && (!d->submit_backend.prepare || !d->submit_backend.launch ||
            !d->submit_backend.poll || !d->submit_backend.release)) { result = INVALID; goto fail; }
        if (s->count && !s->frontend_only) {
            result = d->submit_backend.prepare(d, s, &s->backend_job);
            if (result != VK_SUCCESS) { if (result >= 0) result = INVALID; goto fail; }
            if (!s->backend_job) { result = INVALID; goto fail; }
        }
    }
    if (states) ps5vk_object_free(states, &state_allocator, state_custom);

    struct ps5vk_submission *last = head;
    while (last->next) last = last->next;
    last->fence = fence;
    for (struct ps5vk_submission *s = head; s; s = s->next) {
        for (uint32_t j = 0; j < s->wait_count; ++j) ++s->waits[j]->pending;
        for (uint32_t j = 0; j < s->signal_count; ++j) ++s->signals[j]->pending;
        s->reserved = VK_TRUE;
        pin(s, 1);
    }
    d->queue.next_serial += records;
    d->submission = head;
    if (fence) fence->pending_serial = last->serial;
    d->progress.poll = ps5vk_queue_poll;
    return start_submission(d);

fail:
    if (states) ps5vk_object_free(states, &state_allocator, state_custom);
    discard_chain(d, head);
    return result;
}

#include "vk_queue.h"
#include "vk_buffer_transfer.h"
#include "vk_indirect.h"
#include "vk_query_pool.h"
#include "vk_image_transfer.h"
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN
static int event_operation(int type);
static int frontend_operation(int type);
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
        /* Event host-transition valid usage and lifetime are expressed in
         * terms of a command buffer remaining pending, not merely the short
         * segment that executes the event opcode.  Pin all event references
         * on the 0 -> 1 transition and release them only on 1 -> 0. */
        if (acquire && !c->pending_count) {
            for (uint32_t k = 0; k < c->operation_count; ++k) {
                struct ps5vk_operation *op = &c->operations[k];
                if (!event_operation(op->type)) continue;
                ++op->event->pending;
                if (op->type == PS5VK_EVENT_WAIT) ++op->event->pending_waits;
            }
        }
        uint32_t first = ps5vk_submission_first_operation(s, j);
        uint32_t end = first + ps5vk_submission_operation_count(s, j);
        for (uint32_t k = first; k < end; ++k) {
            struct ps5vk_operation *op = &c->operations[k];
            if (event_operation(op->type)) continue;
            if (op->type == PS5VK_BEGIN_RENDER_PASS) {
                if (acquire) { ++op->render_pass->pending; ++op->framebuffer->pending; }
                else { --op->render_pass->pending; --op->framebuffer->pending; }
                for (uint32_t n = 0; n < op->framebuffer->attachment_count; ++n) {
                    VkImageView view = op->framebuffer->attachments[n];
                    if (acquire) { ++view->pending; ++view->image->pending; }
                    else { --view->pending; --view->image->pending; }
                }
            }
            if (op->type == PS5VK_DRAW || op->type == PS5VK_DRAW_INDEXED ||
                ps5vk_indirect_graphics_operation(op->type)) {
                if (acquire) ++op->pipeline->pending;
                else --op->pipeline->pending;
                for(unsigned set=0;set<PS5VK_MAX_SETS;++set)if(op->sets[set]) {
                    if(acquire)++op->sets[set]->pending;else --op->sets[set]->pending;
                }
            }
            if (op->type != PS5VK_DISPATCH &&
                !ps5vk_indirect_compute_operation(op->type)) continue;
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
            if (!c->pending_count) {
                for (uint32_t k = 0; k < c->operation_count; ++k) {
                    struct ps5vk_operation *op = &c->operations[k];
                    if (!event_operation(op->type)) continue;
                    --op->event->pending;
                    if (op->type == PS5VK_EVENT_WAIT) --op->event->pending_waits;
                }
                c->state = c->usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT ?
                    PS5VK_INVALID : PS5VK_EXECUTABLE;
            }
        }
    }
}

static VkResult start_submission(VkDevice d)
{
    while (d->submission) {
        struct ps5vk_submission *s = d->submission;
        if (!s->waits_consumed) {
            for (uint32_t j = 0; j < s->wait_count; ++j) {
                if (!s->waits[j]->signaled) { d->lost = VK_TRUE; return VK_ERROR_DEVICE_LOST; }
                s->waits[j]->signaled = VK_FALSE;
            }
            s->waits_consumed = VK_TRUE;
        }
        if (s->frontend_only) {
            for (uint32_t b = 0; b < s->count; ++b) {
                VkCommandBuffer command = s->buffers[b];
                uint32_t first = ps5vk_submission_first_operation(s, b);
                uint32_t end = first + ps5vk_submission_operation_count(s, b);
                for (uint32_t k = first; k < end; ++k) {
                    struct ps5vk_operation *op = &command->operations[k];
                    if (op->type == PS5VK_EVENT_WAIT &&
                        !(op->event->host_signaled || op->event->device_signaled)) {
                        return VK_SUCCESS;
                    }
                    if (op->type == PS5VK_EVENT_SET) op->event->device_signaled = VK_TRUE;
                    if (op->type == PS5VK_EVENT_RESET) {
                        op->event->host_signaled = VK_FALSE;
                        op->event->device_signaled = VK_FALSE;
                    }
                    if (ps5vk_buffer_transfer_operation(op->type) &&
                        ps5vk_buffer_transfer_execute(d, op) != VK_SUCCESS) {
                        d->lost = VK_TRUE;
                        return VK_ERROR_DEVICE_LOST;
                    }
                    if (ps5vk_query_operation(op->type) &&
                        ps5vk_query_operation_execute(d, op) != VK_SUCCESS) {
                        d->lost = VK_TRUE;
                        return VK_ERROR_DEVICE_LOST;
                    }
                    if (ps5vk_image_transfer_operation(op->type) &&
                        ps5vk_image_transfer_execute(d, op) != VK_SUCCESS) {
                        d->lost = VK_TRUE;
                        return VK_ERROR_DEVICE_LOST;
                    }
                    if (ps5vk_image_linear_operation(op) &&
                        ps5vk_image_linear_execute(d, op) != VK_SUCCESS) {
                        d->lost = VK_TRUE;
                        return VK_ERROR_DEVICE_LOST;
                    }
                }
            }
            pin(s, 0);
        } else if (s->count) {
            if (!s->backend_job) {
                if (!s->deferred_prepare || !d->submit_backend.prepare ||
                    !d->submit_backend.launch || !d->submit_backend.poll ||
                    !d->submit_backend.release ||
                    d->submit_backend.prepare(d, s, &s->backend_job) != VK_SUCCESS ||
                    !s->backend_job) {
                    d->lost = VK_TRUE;
                    return VK_ERROR_DEVICE_LOST;
                }
            }
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
    if (s->frontend_only) return start_submission(d);
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

VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(VkQueue queue, uint32_t count,
    const VkBindSparseInfo *infos, VkFence fence)
{
    /* No queue family advertises VK_QUEUE_SPARSE_BINDING_BIT. Even count zero
     * cannot waive that validity rule, so reject before inspecting batches or
     * mutating fence/semaphore/submission state. */
    (void)count; (void)infos; (void)fence;
    if (!queue || !queue->device) return INVALID;
    if (queue->device->lost) return VK_ERROR_DEVICE_LOST;
    return VK_ERROR_VALIDATION_FAILED;
}

/* Everything a recorded draw needs to still be true at submission: a live
 * graphics pipeline of this device, resolvable indirect parameters, and
 * descriptor sets that are still the exact generation and signature the
 * pipeline was recorded against. */
static int draw_operation_valid(VkDevice d, const struct ps5vk_operation *op)
{
    if (!op->pipeline || op->pipeline->device != d || !op->pipeline->graphics ||
        !op->pipeline->graphics_state) return 0;
    if (ps5vk_indirect_graphics_operation(op->type) &&
        ps5vk_indirect_validate(d, op) != VK_SUCCESS) return 0;
    if (op->pipeline->set_count > PS5VK_MAX_SETS) return 0;
    for (unsigned set = 0; set < op->pipeline->set_count; ++set)
        if (op->pipeline->sets[set].count &&
            (!op->sets[set] || !op->sets[set]->pool || op->sets[set]->pool->device != d ||
             op->generations[set] != op->sets[set]->generation ||
             memcmp(&op->sets[set]->signature, &op->pipeline->sets[set],
                    sizeof(op->pipeline->sets[set])))) return 0;
    return 1;
}

/* A secondary named inside a render pass must be a continuation recorded for
 * the scope it is about to execute in, and it may carry nothing but draws of
 * that pass: any other command would have no scope to execute in. The
 * framebuffer is optional in the inheritance record, so the null handle is
 * accepted and only a DIFFERENT one is refused. */
static int continuation_child_valid(VkDevice d, VkCommandBuffer child,
    VkRenderPass active, VkFramebuffer framebuffer)
{
    if (!(child->usage & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) ||
        !child->inheritance_valid ||
        !ps5vk_render_pass_compatible(child->inheritance.renderPass, active) ||
        child->inheritance.subpass ||
        (child->inheritance.framebuffer &&
         child->inheritance.framebuffer != framebuffer)) return 0;
    for (unsigned j = 0; j < child->operation_count; ++j) {
        const struct ps5vk_operation *op = &child->operations[j];
        if (op->type != PS5VK_DRAW && op->type != PS5VK_DRAW_INDEXED &&
            !ps5vk_indirect_graphics_operation(op->type)) return 0;
        if (!ps5vk_render_pass_compatible(op->render_pass, active) ||
            (op->framebuffer && op->framebuffer != framebuffer) ||
            !draw_operation_valid(d, op)) return 0;
    }
    return 1;
}

static int command_valid(VkDevice d, VkCommandBuffer c)
{
    if (!c || c->pool->device != d || c->state != PS5VK_EXECUTABLE) return 0;
    VkRenderPass active = NULL;
    VkFramebuffer framebuffer = NULL;
    /* Contents mode of the active pass, read back from the immutable record
     * rather than from recording state. */
    VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE;
    /* DRAW work seen since the pass began - what will execute, not how many
     * commands were written. A vkCmdExecuteCommands marker naming only empty
     * secondaries executes nothing, so counting markers here would accept the
     * zero-body pass that record time refuses. Re-derived from the immutable
     * record so a recording that somehow reaches submission with an empty pass
     * is still refused before any backend sees it. */
    unsigned pass_work = 0;
    for (unsigned j = 0; j < c->operation_count; ++j) {
        const struct ps5vk_operation *op = &c->operations[j];
        if (op->type == PS5VK_EVENT_SET || op->type == PS5VK_EVENT_RESET ||
            op->type == PS5VK_EVENT_WAIT) {
            if (active || !op->event || op->event->device != d ||
                !op->src_stage || !op->dst_stage) return 0;
            continue;
        }
        if (op->type == PS5VK_BEGIN_RENDER_PASS || op->type == PS5VK_DRAW ||
            op->type == PS5VK_DRAW_INDEXED ||
            ps5vk_indirect_graphics_operation(op->type) ||
            op->type == PS5VK_END_RENDER_PASS) {
            if (!d->graphics_enabled || !d->graphics_submit_enabled || !op->render_pass || !op->framebuffer ||
                op->render_pass->device != d || op->framebuffer->device != d) return 0;
            if (op->type == PS5VK_BEGIN_RENDER_PASS) {
                if (active) return 0;
                active = op->render_pass; framebuffer = op->framebuffer;
                contents = op->render_pass_contents;
                pass_work = 0;
                for (uint32_t n = 0; n < framebuffer->attachment_count; ++n) {
                    VkImageView view = framebuffer->attachments[n];
                    void *address; VkDeviceSize bytes;
                    if (!view || view->device != d || !view->image || view->image->display_busy ||
                        ps5vk_image_span(d, view->image, &address, &bytes) != VK_SUCCESS) return 0;
                }
            } else {
                if (active != op->render_pass || framebuffer != op->framebuffer) return 0;
                if (op->type == PS5VK_DRAW || op->type == PS5VK_DRAW_INDEXED ||
                    ps5vk_indirect_graphics_operation(op->type)) {
                    /* A pass begun for secondary contents carries no inline
                     * draws of its own. */
                    if (contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) return 0;
                    if (!draw_operation_valid(d, op)) return 0;
                    ++pass_work;
                } else {
                    if (!pass_work) return 0;
                    active = NULL; framebuffer = NULL;
                    contents = VK_SUBPASS_CONTENTS_INLINE;
                }
            }
            continue;
        }
        if (op->type == PS5VK_EXECUTE_COMMANDS) {
            /* Re-validate the owned child array against live state. Each
             * child's own operations are validated here when it executes
             * inside a render pass, and by the head-of-queue re-validation of
             * its own expanded segment when it executes outside one. */
            VkCommandBuffer const *children = (VkCommandBuffer const *)op->owned_payload;
            if (!children || !op->child_count ||
                op->owned_payload_size != (size_t)op->child_count * sizeof(*children))
                return 0;
            /* The scope must be the one this call was recorded in, and inside
             * a pass that pass must have been begun for secondary contents. */
            if (op->render_pass != active || op->framebuffer != framebuffer) return 0;
            if (active && contents != VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) return 0;
            for (uint32_t n = 0; n < op->child_count; ++n) {
                VkCommandBuffer child = children[n];
                const VkBool32 simultaneous = child &&
                    (child->usage & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) ?
                    VK_TRUE : VK_FALSE;
                /* VUID-vkCmdExecuteCommands-pCommandBuffers-00089 allows a
                 * child in the PENDING or executable state; 00091 restricts
                 * pending to simultaneous-use buffers. Demanding EXECUTABLE
                 * unconditionally would refuse a conformant submission. */
                if (!child || child == c || !child->pool || child->pool->device != d ||
                    child->level != VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
                    (child->state != PS5VK_EXECUTABLE &&
                     !(child->state == PS5VK_PENDING && simultaneous)))
                    return 0;
                if (active) {
                    if (!continuation_child_valid(d, child, active, framebuffer)) return 0;
                    /* A continuation child carries nothing but draws, so its
                     * operation count is the work it contributes - and an
                     * empty child contributes none. */
                    pass_work += child->operation_count;
                } else if (child->usage & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)
                    return 0;
                if (!simultaneous) {
                    if (child->pending_count) return 0;
                    for (uint32_t k = 0; k < n; ++k) if (children[k] == child) return 0;
                }
            }
            continue;
        }
        if (active) return 0;
        if (ps5vk_image_transfer_operation(op->type)) {
            /* Submit-time validation of the recorded image operation; execution
             * re-validates at the head. Lifetime is protected by
             * invalidate_resource, which sees op->image_source/_destination. */
            if (ps5vk_image_transfer_validate(d, op) != VK_SUCCESS) return 0;
            continue;
        }
        if (ps5vk_buffer_transfer_operation(op->type)) {
            if (ps5vk_buffer_transfer_validate(d, op) != VK_SUCCESS) return 0;
            continue;
        }
        if (ps5vk_query_operation(op->type)) {
            if (ps5vk_query_operation_validate(d, op) != VK_SUCCESS) return 0;
            continue;
        }
        if (ps5vk_image_linear_operation(op)) {
            /* Host copies over the padded linear transfer role: no graphics
             * backend submit is involved, so only the role, the resources and
             * the region mapping are validated here. Recorded-order layout
             * state is checked when the segment reaches the head. */
            if (ps5vk_image_linear_validate(d, op) != VK_SUCCESS) return 0;
            continue;
        }
        if(op->type==PS5VK_IMAGE_BARRIER || op->type==PS5VK_COPY_BUFFER_IMAGE ||
           op->type==PS5VK_COPY_IMAGE_BUFFER ||
           op->type==PS5VK_CLEAR_DEPTH_STENCIL_IMAGE) {
            VkImage image=op->type==PS5VK_IMAGE_BARRIER?op->image_barrier.image:
                op->type==PS5VK_CLEAR_DEPTH_STENCIL_IMAGE?op->image_destination:op->copy_image;
            void *address;VkDeviceSize bytes;
            if(!d->graphics_enabled || !d->graphics_submit_enabled || !image || image->display_busy ||
                ps5vk_image_span(d,image,&address,&bytes)!=VK_SUCCESS)return 0;
            /* The recorded depth clear is re-checked against live device state:
             * the role, the owned range payload and the tracked layout. The
             * value itself is already a validated D32 word. */
            if(op->type==PS5VK_CLEAR_DEPTH_STENCIL_IMAGE) {
                const VkImageSubresourceRange *ranges=
                    (const VkImageSubresourceRange *)op->owned_payload;
                if(!ps5vk_depth_clear_image(image) || !op->image_region_count || !ranges ||
                   op->owned_payload_size!=(size_t)op->image_region_count*sizeof(*ranges) ||
                   (op->image_destination_layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                    op->image_destination_layout!=VK_IMAGE_LAYOUT_GENERAL))return 0;
                for(uint32_t r=0;r<op->image_region_count;++r)
                    if(ranges[r].aspectMask!=VK_IMAGE_ASPECT_DEPTH_BIT ||
                       ranges[r].baseMipLevel || ranges[r].baseArrayLayer ||
                       (ranges[r].levelCount!=1 &&
                        ranges[r].levelCount!=VK_REMAINING_MIP_LEVELS) ||
                       (ranges[r].layerCount!=1 &&
                        ranges[r].layerCount!=VK_REMAINING_ARRAY_LAYERS))return 0;
            }
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
        if (op->type != PS5VK_DISPATCH &&
            !ps5vk_indirect_compute_operation(op->type)) return 0;
        if (ps5vk_indirect_compute_operation(op->type) &&
            ps5vk_indirect_validate(d, op) != VK_SUCCESS) return 0;
        if (!op->pipeline || op->pipeline->graphics ||
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

static struct ps5vk_submission *allocate_submission(VkDevice d, size_t refs,
    VkResult *result)
{
    if (refs > (SIZE_MAX - sizeof(struct ps5vk_submission)) / sizeof(VkSemaphore)) {
        *result = VK_ERROR_OUT_OF_HOST_MEMORY; return NULL;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    struct ps5vk_submission *submission = ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, NULL,
        sizeof(*submission) + refs * sizeof(VkSemaphore),
        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &saved, &custom);
    if (!submission) { *result = VK_ERROR_OUT_OF_HOST_MEMORY; return NULL; }
    submission->allocator = saved; submission->custom_allocator = custom;
    submission->waits = (VkSemaphore *)(submission + 1);
    submission->signals = submission->waits;
    return submission;
}

static int event_operation(int type)
{
    return type == PS5VK_EVENT_SET || type == PS5VK_EVENT_RESET ||
        type == PS5VK_EVENT_WAIT;
}

static int frontend_operation(int type)
{
    return event_operation(type) ||
        ps5vk_buffer_transfer_operation((enum ps5vk_operation_type)type) ||
        ps5vk_query_operation((enum ps5vk_operation_type)type) ||
        ps5vk_image_transfer_operation((enum ps5vk_operation_type)type);
}

/* Frontend work of one recorded operation: the type-level frontend families plus
 * the pure transfer role's image <-> buffer transfers and layout bookkeeping,
 * which are host copies over the same padded linear layout. The tiled
 * render-target readback and the sampled upload keep their GPU path. */
static int frontend_record(const struct ps5vk_operation *op)
{
    return frontend_operation(op->type) || ps5vk_image_linear_operation(op);
}

static int deferred_boundary(int type)
{ return ps5vk_indirect_compute_operation((enum ps5vk_operation_type)type); }

/* vkCmdExecuteCommands names children instead of carrying work, so it ends the
 * primary's current segment: the children are expanded into their own segments
 * in recorded order and the primary resumes after them. */
static int execute_boundary(int type)
{ return type == PS5VK_EXECUTE_COMMANDS; }

static VkBool32 range_has_indirect(const VkCommandBuffer command,
    uint32_t first, uint32_t count)
{
    for (uint32_t k = first; k < first + count; ++k)
        if (ps5vk_indirect_operation(command->operations[k].type)) return VK_TRUE;
    return VK_FALSE;
}

/* Emit one ordered segment naming exactly one command buffer and range. */
static VkResult emit_segment(VkDevice d, VkCommandBuffer command, size_t refs,
    uint32_t begin, uint32_t count, VkBool32 frontend_only, VkBool32 deferred,
    struct ps5vk_submission **first, struct ps5vk_submission **last,
    struct ps5vk_submission ***tail, uint32_t *segment_count)
{
    VkResult result = VK_SUCCESS;
    struct ps5vk_submission *s = allocate_submission(d, refs, &result);
    if (!s) return result;
    s->count = 1; s->buffers[0] = command;
    s->first_operation[0] = (uint16_t)begin;
    s->operation_count[0] = (uint16_t)count;
    s->frontend_only = frontend_only;
    s->deferred_prepare = deferred;
    if (*segment_count == UINT32_MAX) { free_submission(s); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (!*first) *first = s;
    **tail = s; *tail = &s->next; *last = s; ++*segment_count;
    return VK_SUCCESS;
}

/* Index of the PS5VK_END_RENDER_PASS that closes the pass opened at `begin`,
 * or the operation count when the recording carries none. */
static uint32_t render_pass_end(const VkCommandBuffer command, uint32_t begin)
{
    uint32_t end = begin + 1;
    while (end < command->operation_count &&
           command->operations[end].type != PS5VK_END_RENDER_PASS) ++end;
    return end;
}

static VkBool32 range_names_children(const VkCommandBuffer command,
    uint32_t first, uint32_t count)
{
    for (uint32_t k = first; k < first + count; ++k)
        if (command->operations[k].type == PS5VK_EXECUTE_COMMANDS) return VK_TRUE;
    return VK_FALSE;
}

/* Emit ONE segment for a whole render pass whose work is named by secondaries.
 *
 * A render pass is a single scope and the backend builds one command stream
 * for it, so it cannot be split the way vkCmdExecuteCommands splits work
 * OUTSIDE a pass: the primary's BEGIN..END range becomes buffer 0 and every
 * named child follows in recorded order with its own range. Nothing is copied
 * or flattened into the primary, so each buffer keeps its identity and pin()
 * still does per-buffer pending ownership, one-time-submit consumption and
 * reset/free protection exactly as it does for a separate segment. Retiring
 * together is the correct lifetime here: the children ARE the pass. */
static VkResult emit_render_pass_segment(VkDevice d, VkCommandBuffer command,
    size_t refs, uint32_t begin, uint32_t count,
    struct ps5vk_submission **first, struct ps5vk_submission **last,
    struct ps5vk_submission ***tail, uint32_t *segment_count)
{
    VkResult result = VK_SUCCESS;
    struct ps5vk_submission *s = allocate_submission(d, refs, &result);
    if (!s) return result;
    s->count = 1; s->buffers[0] = command;
    s->first_operation[0] = (uint16_t)begin;
    s->operation_count[0] = (uint16_t)count;
    VkBool32 indirect = range_has_indirect(command, begin, count);
    for (uint32_t k = begin; k < begin + count; ++k) {
        const struct ps5vk_operation *op = &command->operations[k];
        if (op->type != PS5VK_EXECUTE_COMMANDS) continue;
        VkCommandBuffer const *children = (VkCommandBuffer const *)op->owned_payload;
        if (!children || !op->child_count ||
            op->owned_payload_size != (size_t)op->child_count * sizeof(*children))
            { free_submission(s); return VK_ERROR_UNKNOWN; }
        for (uint32_t n = 0; n < op->child_count; ++n) {
            /* The submission record holds a bounded number of buffers. This is
             * a real limit of this representation, reported as a resource
             * failure rather than silently dropping a child. */
            if (!children[n]) { free_submission(s); return VK_ERROR_UNKNOWN; }
            if (s->count == PS5VK_MAX_SUBMITTED_BUFFERS)
                { free_submission(s); return VK_ERROR_OUT_OF_HOST_MEMORY; }
            s->buffers[s->count] = children[n];
            s->first_operation[s->count] = 0;
            s->operation_count[s->count] = (uint16_t)children[n]->operation_count;
            ++s->count;
            indirect |= range_has_indirect(children[n], 0, children[n]->operation_count);
        }
    }
    s->frontend_only = VK_FALSE;
    s->deferred_prepare = indirect;
    if (*segment_count == UINT32_MAX) { free_submission(s); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (!*first) *first = s;
    **tail = s; *tail = &s->next; *last = s; ++*segment_count;
    return VK_SUCCESS;
}

/* Walk ONE command buffer's operations and emit its ordered segments.
 *
 * Shared by a primary and, recursively, by every secondary it names, so a
 * child is segmented by exactly the same rules instead of being classified as
 * a whole. That matters because a child is not uniform: a legal event followed
 * by a barrier is part frontend and part backend, and judging the child by its
 * first operation would execute one half and silently drop the other.
 * Children cannot nest, so the recursion is one level deep. */
static VkResult segment_operations(VkDevice d, VkCommandBuffer command, size_t refs,
    struct ps5vk_submission **first, struct ps5vk_submission **last,
    struct ps5vk_submission ***tail, uint32_t *segment_count)
{
    /* An empty buffer still gets a segment. Executing it is a no-op, but its
     * pending ownership and one-time-submit consumption are not, and skipping
     * it would silently lose both. */
    if (!command->operation_count)
        return emit_segment(d, command, refs, 0, 0, VK_TRUE, VK_FALSE,
                            first, last, tail, segment_count);
    uint32_t operation = 0;
    while (operation < command->operation_count) {
        uint32_t begin = operation;
        /* A render pass whose work is named by secondaries is emitted whole,
         * before the generic boundary rules get a chance to split it. */
        if (command->operations[operation].type == PS5VK_BEGIN_RENDER_PASS) {
            uint32_t end = render_pass_end(command, operation);
            if (end < command->operation_count &&
                range_names_children(command, operation, end - operation + 1)) {
                VkResult composed = emit_render_pass_segment(d, command, refs,
                    operation, end - operation + 1, first, last, tail, segment_count);
                if (composed != VK_SUCCESS) return composed;
                operation = end + 1;
                continue;
            }
        }
        VkBool32 frontend = frontend_record(&command->operations[operation]);
        VkBool32 deferred = deferred_boundary(command->operations[operation].type);
        VkBool32 execute = execute_boundary(command->operations[operation].type);
        VkResult rc;
        if (frontend || deferred || execute) ++operation;
        else while (operation < command->operation_count &&
            !frontend_record(&command->operations[operation]) &&
            !execute_boundary(command->operations[operation].type) &&
            !deferred_boundary(command->operations[operation].type)) ++operation;
        if (execute) {
            const struct ps5vk_operation *ex = &command->operations[begin];
            VkCommandBuffer const *children = (VkCommandBuffer const *)ex->owned_payload;
            if (!children || !ex->child_count ||
                ex->owned_payload_size != (size_t)ex->child_count * sizeof(*children))
                return VK_ERROR_UNKNOWN;
            for (uint32_t n = 0; n < ex->child_count; ++n) {
                if (!children[n]) return VK_ERROR_UNKNOWN;
                rc = segment_operations(d, children[n], refs, first, last, tail,
                                        segment_count);
                if (rc != VK_SUCCESS) return rc;
            }
            /* Then a segment for the primary covering just this operation. It
             * carries no work, but it is what keeps the PARENT pinned: without
             * it a primary whose only operation is vkCmdExecuteCommands never
             * appears in any segment, so pin() never sees it and its pending
             * state, reset/free protection and one-time-submit consumption are
             * all wrong. Emitting it AFTER the children also makes the parent
             * outlive them, because segments retire in order. */
            rc = emit_segment(d, command, refs, begin, 1, VK_TRUE, VK_FALSE,
                              first, last, tail, segment_count);
            if (rc != VK_SUCCESS) return rc;
            continue;
        }
        rc = emit_segment(d, command, refs, begin, operation - begin, frontend,
            !frontend && range_has_indirect(command, begin, operation - begin),
            first, last, tail, segment_count);
        if (rc != VK_SUCCESS) return rc;
    }
    return VK_SUCCESS;
}

static VkResult expand_records(VkDevice d, struct ps5vk_submission *original,
    struct ps5vk_submission **expanded, uint32_t *segment_count)
{
    struct ps5vk_submission *head = NULL, **tail = &head;
    VkResult result = VK_SUCCESS;
    for (struct ps5vk_submission *record = original; record; record = record->next) {
        size_t refs = (size_t)record->wait_count + record->signal_count;
        VkBool32 contains_special = VK_FALSE;
        for (uint32_t b = 0; b < record->count; ++b)
            for (uint32_t k = 0; k < record->buffers[b]->operation_count; ++k)
                contains_special |= frontend_record(&record->buffers[b]->operations[k]) ||
                    execute_boundary(record->buffers[b]->operations[k].type) ||
                    ps5vk_indirect_operation(record->buffers[b]->operations[k].type);
        struct ps5vk_submission *first = NULL, *last = NULL;
        if (!contains_special) {
            last = allocate_submission(d, refs, &result);
            if (!last) goto fail;
            last->count = record->count;
            for (uint32_t b = 0; b < record->count; ++b) last->buffers[b] = record->buffers[b];
            if (*segment_count == UINT32_MAX) {
                free_submission(last); result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail;
            }
            first = last; *tail = last; tail = &last->next; ++*segment_count;
        } else {
            for (uint32_t b = 0; b < record->count; ++b) {
                result = segment_operations(d, record->buffers[b], refs,
                                            &first, &last, &tail, segment_count);
                if (result != VK_SUCCESS) goto fail;
            }
            if (!first) {
                last = allocate_submission(d, refs, &result); if (!last) goto fail;
                if (*segment_count == UINT32_MAX) {
                    free_submission(last); result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail;
                }
                first = last; *tail = last; tail = &last->next; ++*segment_count;
            }
        }
        first->wait_count = record->wait_count;
        memcpy(first->waits, record->waits, record->wait_count * sizeof(VkSemaphore));
        last->signal_count = record->signal_count;
        last->signals = (VkSemaphore *)(last + 1) + last->wait_count;
        memcpy(last->signals, record->signals, record->signal_count * sizeof(VkSemaphore));
    }
    *expanded = head; return VK_SUCCESS;
fail:
    discard_chain(d, head); return result;
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
            /* Only a primary is submittable. A secondary reaches the queue
             * through vkCmdExecuteCommands, which is still fail-closed, so
             * submitting one directly is refused rather than silently run. */
            if (command && command->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
                return INVALID;
            if (command && command->pool && command->pool->device == d &&
                command->state == PS5VK_PENDING &&
                !(command->usage & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT))
                return INVALID;
        }
    }
    if (!d->queue.next_serial) return INVALID;

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
        struct ps5vk_submission *s = allocate_submission(d, refs, &result);
        if (!s) goto fail;
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
    }

    struct ps5vk_submission *expanded = NULL;
    uint32_t segments = 0;
    result = expand_records(d, head, &expanded, &segments);
    if (result != VK_SUCCESS) goto fail;
    discard_chain(d, head); head = expanded;
    if (!segments || segments > UINT64_MAX - d->queue.next_serial) {
        result = INVALID; goto fail;
    }
    uint32_t segment = 0;
    VkBool32 seen_frontend = VK_FALSE;
    for (struct ps5vk_submission *s = head; s; s = s->next, ++segment) {
        s->serial = d->queue.next_serial + segment;
        if (s->frontend_only) seen_frontend = VK_TRUE;
        else if (seen_frontend) s->deferred_prepare = VK_TRUE;
        if (s->count && !s->frontend_only && !s->deferred_prepare) {
            if (!d->submit_backend.prepare || !d->submit_backend.launch ||
                !d->submit_backend.poll || !d->submit_backend.release) {
                result = INVALID; goto fail;
            }
            result = d->submit_backend.prepare(d, s, &s->backend_job);
            if (result != VK_SUCCESS) { if (result >= 0) result = INVALID; goto fail; }
            if (!s->backend_job) { result = INVALID; goto fail; }
        }
    }
    if (states) { ps5vk_object_free(states, &state_allocator, state_custom); states = NULL; }

    struct ps5vk_submission *last = head;
    while (last->next) last = last->next;
    last->fence = fence;
    for (struct ps5vk_submission *s = head; s; s = s->next) {
        for (uint32_t j = 0; j < s->wait_count; ++j) ++s->waits[j]->pending;
        for (uint32_t j = 0; j < s->signal_count; ++j) ++s->signals[j]->pending;
        s->reserved = VK_TRUE;
        pin(s, 1);
    }
    d->queue.next_serial += segments;
    d->submission = head;
    if (fence) fence->pending_serial = last->serial;
    d->progress.poll = ps5vk_queue_poll;
    return start_submission(d);

fail:
    if (states) ps5vk_object_free(states, &state_allocator, state_custom);
    discard_chain(d, head);
    return result;
}

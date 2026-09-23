/*
 * Executable vkCmdExecuteCommands: ordering, lifetime and fail-closed edges.
 *
 * The contract under test is that a named secondary is EXPANDED into its own
 * submission segment rather than flattened into the primary, so the child keeps
 * its identity, its pending ownership and its reuse rules.
 *
 * The oracle is deliberately EXECUTION, not segmentation. A secondary that
 * records only vkCmdSetEvent is made of frontend operations and runs host-side
 * at the queue head without ever reaching a backend, so watching a backend
 * callback would observe nothing and prove nothing. Each child therefore owns
 * a VkEvent, and the assertion is that vkGetEventStatus moves RESET -> SET.
 */
#include "vk_command.h"
#include "vk_queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned prepares, fail_prepare_at;
/* The mock must report the segment's OWN serial as completed. Reporting a
 * constant works only while a submission produces one segment; secondary
 * expansion produces several, and a stale serial reads as device loss. */
static VkResult prepare(VkDevice d, const struct ps5vk_submission *s, void **job)
{
    (void)d;
    if (fail_prepare_at && ++prepares == fail_prepare_at) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t *serial = malloc(sizeof(*serial));
    if (!serial) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *serial = s->serial; *job = serial;
    return VK_SUCCESS;
}
static VkResult launch(VkDevice d, void *job) { (void)d; (void)job; return VK_SUCCESS; }
static VkResult poll_backend(VkDevice d, void *job, uint64_t *completed)
{ (void)d; *completed = *(uint64_t *)job; return VK_SUCCESS; }
static void release(VkDevice d, void *job) { (void)d; free(job); }
static uint64_t clock_ns(void *ctx) { (void)ctx; static uint64_t t; return ++t; }
static void pause_wait(void *ctx, uint64_t ns) { (void)ctx; (void)ns; }

static VkCommandBuffer allocate(VkDevice d, VkCommandPool p, VkCommandBufferLevel level)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p, .level = level, .commandBufferCount = 1};
    VkCommandBuffer c;
    assert(vkAllocateCommandBuffers(d, &info, &c) == VK_SUCCESS);
    return c;
}
static VkCommandBufferInheritanceInfo inheritance(void)
{ return (VkCommandBufferInheritanceInfo){
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}; }

/* A secondary carrying one recorded operation, so its execution is observable. */
static VkCommandBuffer child_with_event(VkDevice d, VkCommandPool p, VkEvent event,
                                        VkCommandBufferUsageFlags usage)
{
    VkCommandBuffer child = allocate(d, p, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    VkCommandBufferInheritanceInfo inherit = inheritance();
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = usage, .pInheritanceInfo = &inherit};
    assert(vkBeginCommandBuffer(child, &begin) == VK_SUCCESS);
    vkCmdSetEvent(child, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    assert(vkEndCommandBuffer(child) == VK_SUCCESS);
    assert(child->operation_count == 1);
    return child;
}
static VkResult image_requirements(VkDevice d, const VkImageCreateInfo *i, VkMemoryRequirements *r)
{ (void)d; (void)i; *r = (VkMemoryRequirements){256, 256, 1}; return VK_SUCCESS; }
static VkResult memory_allocate(void *ctx, VkDeviceSize n, void **a, void **b)
{ (void)ctx; *a = *b = calloc(1, n); return *a ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void memory_release(void *ctx, void *b) { (void)ctx; free(b); }
static VkResult memory_sync(void *ctx, void *b, VkDeviceSize offset, VkDeviceSize bytes)
{ (void)ctx; (void)b; (void)offset; (void)bytes; return VK_SUCCESS; }

static VkCommandBuffer begun_primary(VkDevice d, VkCommandPool p)
{
    VkCommandBuffer c = allocate(d, p, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(c, &begin) == VK_SUCCESS);
    return c;
}

int main(void)
{
    struct VkDevice_T d = {.progress = {NULL, ps5vk_queue_poll, clock_ns, pause_wait},
        .submit_backend = {prepare, launch, poll_backend, release}};
    d.queue.device = &d; d.queue.next_serial = 1;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    assert(vkCreateCommandPool(&d, &pi, NULL, &pool) == VK_SUCCESS);
    VkEventCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent first, second;
    assert(vkCreateEvent(&d, &ei, NULL, &first) == VK_SUCCESS);
    assert(vkCreateEvent(&d, &ei, NULL, &second) == VK_SUCCESS);

    /* --- the named children execute, in call order ------------------------ */
    VkCommandBuffer a = child_with_event(&d, pool, first, 0);
    VkCommandBuffer b = child_with_event(&d, pool, second, 0);
    VkCommandBuffer primary = begun_primary(&d, pool);
    const VkCommandBuffer both[2] = {a, b};
    vkCmdExecuteCommands(primary, 2, both);
    assert(primary->state == PS5VK_RECORDING && primary->operation_count == 1);
    assert(primary->operations[0].type == PS5VK_EXECUTE_COMMANDS);
    assert(primary->operations[0].child_count == 2);
    /* The array is owned: a later caller mutation cannot change who executes. */
    const VkCommandBuffer *owned = primary->operations[0].owned_payload;
    assert(owned && owned != both && owned[0] == a && owned[1] == b);
    assert(vkEndCommandBuffer(primary) == VK_SUCCESS);

    assert(vkGetEventStatus(&d, first) == VK_EVENT_RESET);
    assert(vkGetEventStatus(&d, second) == VK_EVENT_RESET);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &primary};
    assert(vkQueueSubmit(&d.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    /* Both children's work ran, and the primary was never rewritten to carry
     * it: it still holds exactly the one naming operation. */
    assert(vkGetEventStatus(&d, first) == VK_EVENT_SET);
    assert(vkGetEventStatus(&d, second) == VK_EVENT_SET);
    assert(primary->operation_count == 1 &&
           primary->operations[0].type == PS5VK_EXECUTE_COMMANDS);
    /* Children are reusable afterwards; nothing consumed them. */
    assert(a->state == PS5VK_EXECUTABLE && !a->pending_count);
    assert(b->state == PS5VK_EXECUTABLE && !b->pending_count);

    /* --- a one-time-submit child is consumed by executing ----------------- */
    VkEvent once_event;
    assert(vkCreateEvent(&d, &ei, NULL, &once_event) == VK_SUCCESS);
    VkCommandBuffer once = child_with_event(&d, pool, once_event,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    assert(vkResetCommandBuffer(primary, 0) == VK_SUCCESS);
    primary = begun_primary(&d, pool);
    vkCmdExecuteCommands(primary, 1, &once);
    assert(vkEndCommandBuffer(primary) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(vkGetEventStatus(&d, once_event) == VK_EVENT_SET);
    /* The expansion did not bypass pending accounting: the child is consumed
     * exactly as a primary would be, and can no longer be named. */
    assert(once->state == PS5VK_INVALID && !once->pending_count);
    VkCommandBuffer after = begun_primary(&d, pool);
    vkCmdExecuteCommands(after, 1, &once);
    assert(after->state == PS5VK_INVALID && !after->operation_count);

    /* --- duplicates need simultaneous use --------------------------------- */
    VkCommandBuffer dup = begun_primary(&d, pool);
    const VkCommandBuffer twice[2] = {a, a};
    vkCmdExecuteCommands(dup, 2, twice);
    assert(dup->state == PS5VK_INVALID && !dup->operation_count);

    VkEvent shared_event;
    assert(vkCreateEvent(&d, &ei, NULL, &shared_event) == VK_SUCCESS);
    VkCommandBuffer shared = child_with_event(&d, pool, shared_event,
        VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    VkCommandBuffer simul = begun_primary(&d, pool);
    const VkCommandBuffer shared_twice[2] = {shared, shared};
    vkCmdExecuteCommands(simul, 2, shared_twice);
    assert(simul->state == PS5VK_RECORDING && simul->operation_count == 1 &&
           simul->operations[0].child_count == 2);
    assert(vkEndCommandBuffer(simul) == VK_SUCCESS);
    VkSubmitInfo simul_si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &simul};
    assert(vkQueueSubmit(&d.queue, 1, &simul_si, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(vkGetEventStatus(&d, shared_event) == VK_EVENT_SET);
    /* Named twice and still reusable: simultaneous use is not consumption. */
    assert(shared->state == PS5VK_EXECUTABLE && !shared->pending_count);

    /* --- a MIXED child is segmented per operation, not classified whole ---
     * An event (frontend) followed by a barrier (backend) must have BOTH
     * halves executed. Judging the child by its first operation would run the
     * event and silently drop the barrier, which no uniform event-only child
     * can reveal. */
    VkEvent mixed_event;
    assert(vkCreateEvent(&d, &ei, NULL, &mixed_event) == VK_SUCCESS);
    VkCommandBuffer mixed = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    {
        VkCommandBufferInheritanceInfo mi = inheritance();
        VkCommandBufferBeginInfo mb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pInheritanceInfo = &mi};
        assert(vkBeginCommandBuffer(mixed, &mb) == VK_SUCCESS);
        vkCmdSetEvent(mixed, mixed_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        vkCmdPipelineBarrier(mixed, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
        assert(vkEndCommandBuffer(mixed) == VK_SUCCESS);
        assert(mixed->operation_count == 2);
        assert(mixed->operations[0].type == PS5VK_EVENT_SET);
        assert(mixed->operations[1].type == PS5VK_BARRIER);
    }
    VkCommandBuffer mixed_parent = begun_primary(&d, pool);
    vkCmdExecuteCommands(mixed_parent, 1, &mixed);
    assert(vkEndCommandBuffer(mixed_parent) == VK_SUCCESS);
    VkSubmitInfo mixed_si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &mixed_parent};
    assert(vkQueueSubmit(&d.queue, 1, &mixed_si, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    /* The frontend half ran, and the whole submission completed rather than
     * dropping the backend half. */
    assert(vkGetEventStatus(&d, mixed_event) == VK_EVENT_SET);
    assert(mixed->state == PS5VK_EXECUTABLE && !mixed->pending_count);
    assert(mixed_parent->state == PS5VK_EXECUTABLE && !mixed_parent->pending_count);

    /* --- the PARENT is pinned even when it only names children ------------
     * A primary whose sole operation is vkCmdExecuteCommands must still become
     * pending and must still be consumed by one-time-submit; otherwise
     * reset/free protection silently disappears. */
    VkEvent parent_event;
    assert(vkCreateEvent(&d, &ei, NULL, &parent_event) == VK_SUCCESS);
    VkCommandBuffer only_child = child_with_event(&d, pool, parent_event,
        VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    VkCommandBuffer one_time_parent = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    {
        VkCommandBufferBeginInfo ob = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        assert(vkBeginCommandBuffer(one_time_parent, &ob) == VK_SUCCESS);
        vkCmdExecuteCommands(one_time_parent, 1, &only_child);
        assert(one_time_parent->operation_count == 1);
        assert(vkEndCommandBuffer(one_time_parent) == VK_SUCCESS);
    }
    VkSubmitInfo parent_si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &one_time_parent};
    assert(vkQueueSubmit(&d.queue, 1, &parent_si, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(vkGetEventStatus(&d, parent_event) == VK_EVENT_SET);
    /* Consumed exactly as any one-time-submit primary would be. */
    assert(one_time_parent->state == PS5VK_INVALID && !one_time_parent->pending_count);

    /* --- an EMPTY child still has lifecycle ------------------------------- */
    VkCommandBuffer empty = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    {
        VkCommandBufferInheritanceInfo eih = inheritance();
        VkCommandBufferBeginInfo eb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = &eih};
        assert(vkBeginCommandBuffer(empty, &eb) == VK_SUCCESS);
        assert(vkEndCommandBuffer(empty) == VK_SUCCESS);
        assert(!empty->operation_count);
    }
    VkCommandBuffer empty_parent = begun_primary(&d, pool);
    vkCmdExecuteCommands(empty_parent, 1, &empty);
    assert(vkEndCommandBuffer(empty_parent) == VK_SUCCESS);
    VkSubmitInfo empty_si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &empty_parent};
    assert(vkQueueSubmit(&d.queue, 1, &empty_si, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    /* Executing nothing is still executing: the one-time child is consumed. */
    assert(empty->state == PS5VK_INVALID && !empty->pending_count);

    /* --- resetting a referenced child poisons its parent ------------------
     * The recorded array holds raw references; a child that is reset,
     * re-recorded or freed before the parent submits would leave them
     * dangling, so the parent is invalidated instead. */
    VkEvent dep_event;
    assert(vkCreateEvent(&d, &ei, NULL, &dep_event) == VK_SUCCESS);
    VkCommandBuffer dep_child = child_with_event(&d, pool, dep_event, 0);
    VkCommandBuffer dep_parent = begun_primary(&d, pool);
    vkCmdExecuteCommands(dep_parent, 1, &dep_child);
    assert(vkEndCommandBuffer(dep_parent) == VK_SUCCESS);
    assert(dep_parent->state == PS5VK_EXECUTABLE);
    assert(vkResetCommandBuffer(dep_child, 0) == VK_SUCCESS);
    assert(dep_parent->state == PS5VK_INVALID);
    /* And an invalid parent cannot be submitted. */
    VkSubmitInfo dep_si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &dep_parent};
    assert(vkQueueSubmit(&d.queue, 1, &dep_si, VK_NULL_HANDLE) != VK_SUCCESS);

    /* Freeing a referenced child does the same, before the object goes away,
     * so a later submit cannot read freed memory. */
    VkCommandBuffer freed_child = child_with_event(&d, pool, dep_event, 0);
    VkCommandBuffer freed_parent = begun_primary(&d, pool);
    vkCmdExecuteCommands(freed_parent, 1, &freed_child);
    assert(vkEndCommandBuffer(freed_parent) == VK_SUCCESS);
    vkFreeCommandBuffers(&d, pool, 1, &freed_child);
    assert(freed_parent->state == PS5VK_INVALID);

    /* --- a PENDING simultaneous-use child is legal ------------------------
     * VUID-vkCmdExecuteCommands-pCommandBuffers-00089 allows a child in the
     * pending OR executable state, and 00091 restricts pending only for
     * buffers WITHOUT simultaneous use. Demanding executable unconditionally
     * would refuse a conformant call. Forcing the pending state directly is
     * how the host harness reaches it without a live GPU. */
    {
        VkEvent pending_event;
        assert(vkCreateEvent(&d, &ei, NULL, &pending_event) == VK_SUCCESS);
        VkCommandBuffer simul_child = child_with_event(&d, pool, pending_event,
            VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
        const enum ps5vk_command_state saved = simul_child->state;
        simul_child->state = PS5VK_PENDING; simul_child->pending_count = 1;
        VkCommandBuffer accepts = begun_primary(&d, pool);
        vkCmdExecuteCommands(accepts, 1, &simul_child);
        assert(accepts->state == PS5VK_RECORDING && accepts->operation_count == 1);
        /* The same state WITHOUT simultaneous use is refused, per 00091. */
        VkCommandBuffer plain = child_with_event(&d, pool, pending_event, 0);
        plain->state = PS5VK_PENDING; plain->pending_count = 1;
        VkCommandBuffer refuses = begun_primary(&d, pool);
        vkCmdExecuteCommands(refuses, 1, &plain);
        assert(refuses->state == PS5VK_INVALID && !refuses->operation_count);
        plain->state = saved; plain->pending_count = 0;
        simul_child->state = saved; simul_child->pending_count = 0;
        vkDestroyEvent(&d, pending_event, NULL);
    }

    /* --- fail-closed edges, each leaving nothing recorded ----------------- */
    VkCommandBuffer probe;
    /* nesting: a secondary may not execute anything */
    VkCommandBuffer nester = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    VkCommandBufferInheritanceInfo inherit = inheritance();
    VkCommandBufferBeginInfo sbegin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pInheritanceInfo = &inherit};
    assert(vkBeginCommandBuffer(nester, &sbegin) == VK_SUCCESS);
    vkCmdExecuteCommands(nester, 1, &a);
    assert(nester->state == PS5VK_INVALID && !nester->operation_count);

    /* a primary child is not executable as a secondary */
    VkCommandBuffer wrong_level = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    VkCommandBufferBeginInfo pbegin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(wrong_level, &pbegin) == VK_SUCCESS);
    assert(vkEndCommandBuffer(wrong_level) == VK_SUCCESS);
    probe = begun_primary(&d, pool);
    vkCmdExecuteCommands(probe, 1, &wrong_level);
    assert(probe->state == PS5VK_INVALID && !probe->operation_count);

    /* a child still recording is not executable */
    VkCommandBuffer recording = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    assert(vkBeginCommandBuffer(recording, &sbegin) == VK_SUCCESS);
    probe = begun_primary(&d, pool);
    vkCmdExecuteCommands(probe, 1, &recording);
    assert(probe->state == PS5VK_INVALID && !probe->operation_count);

    /* self-reference, empty and null arrays, and an over-long list */
    probe = begun_primary(&d, pool);
    vkCmdExecuteCommands(probe, 1, &probe);
    assert(probe->state == PS5VK_INVALID && !probe->operation_count);
    probe = begun_primary(&d, pool);
    vkCmdExecuteCommands(probe, 0, &a);
    assert(probe->state == PS5VK_INVALID && !probe->operation_count);
    probe = begun_primary(&d, pool);
    vkCmdExecuteCommands(probe, 1, NULL);
    assert(probe->state == PS5VK_INVALID && !probe->operation_count);
    /* A long but legal list is ACCEPTED: there is no invented cap, and the
     * segment list is dynamically allocated. */
    {
        enum { MANY = 24 };
        VkCommandBuffer many[MANY];
        for (unsigned i = 0; i < MANY; ++i) many[i] = shared;
        probe = begun_primary(&d, pool);
        vkCmdExecuteCommands(probe, MANY, many);
        assert(probe->state == PS5VK_RECORDING && probe->operation_count == 1 &&
               probe->operations[0].child_count == MANY);
    }

    /* Continuation needs a real inherited scope: the flag with no render pass
     * named is refused, and so is naming such a buffer at all. */
    VkCommandBuffer continued = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    VkCommandBufferBeginInfo cbegin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
        .pInheritanceInfo = &inherit};
    assert(vkBeginCommandBuffer(continued, &cbegin) != VK_SUCCESS);
    probe = begun_primary(&d, pool);
    vkCmdExecuteCommands(probe, 1, &continued);
    assert(probe->state == PS5VK_INVALID && !probe->operation_count);

    /* a secondary is still not directly submittable */
    VkSubmitInfo bad = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &a};
    assert(vkQueueSubmit(&d.queue, 1, &bad, VK_NULL_HANDLE) != VK_SUCCESS);

    /* --- rollback: a failing prepare leaves nothing pending --------------- */
    VkCommandBuffer rollback = begun_primary(&d, pool);
    vkCmdExecuteCommands(rollback, 1, &b);
    assert(vkEndCommandBuffer(rollback) == VK_SUCCESS);
    VkSubmitInfo rb = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &rollback};
    prepares = 0; fail_prepare_at = 1;
    if (vkQueueSubmit(&d.queue, 1, &rb, VK_NULL_HANDLE) == VK_SUCCESS)
        (void)vkQueueWaitIdle(&d.queue);
    fail_prepare_at = 0;
    assert(!b->pending_count && !rollback->pending_count);

    /* ====================================================================
     * S3: execution INSIDE a render pass.
     *
     * The oracle here is the SHAPE OF THE SEGMENT, and that is not a weaker
     * oracle than S2's executed event: a render pass is one scope and the
     * backend builds one command stream for it, so proving that the parent's
     * BEGIN..END range and every named child land in ONE segment, in recorded
     * order, with each buffer keeping its own range, is exactly the property
     * that makes the pass executable at all. Splitting it would submit a
     * begin, a draw and an end as three unrelated jobs.
     * ==================================================================== */
    d.graphics_enabled = VK_TRUE; d.graphics_submit_enabled = VK_TRUE;
    d.image_requirements = image_requirements;
    d.memory.allocate = memory_allocate; d.memory.release = memory_release;
    d.memory.flush = memory_sync; d.memory.invalidate = memory_sync;
    d.max_allocation = 4096; d.noncoherent_atom = 64;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {4,4,1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImage image; VkDeviceMemory memory;
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 256};
    assert(vkCreateImage(&d, &ii, NULL, &image) == VK_SUCCESS);
    assert(vkAllocateMemory(&d, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(&d, image, memory, 0) == VK_SUCCESS);
    struct VkImageView_T view = {.device = &d, .image = image};
    /* Each synthetic pass owns its own arrays: the object holds POINTERS to
     * them now, so copying the struct would share one attachment array between
     * two passes and a mutation meant for one would change both. */
    VkAttachmentDescription bgra_attachment[1] = {
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT}};
    struct ps5vk_subpass colour_only[1] = {
        {.color[0] = {.attachment = 0}, .color_count = 1, .depth = {.attachment = VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T pass = {.device = &d, .attachment_count = 1, .subpass_count = 1,
        .attachments = bgra_attachment, .subpasses = colour_only};
    /* A DIFFERENT object with the same shape: compatibility is defined by
     * attachment formats, sample counts and references, not by identity, so a
     * secondary recorded against this one must be accepted by the pass above. */
    VkAttachmentDescription twin_attachment[1] = {bgra_attachment[0]};
    struct ps5vk_subpass twin_subpasses[1] = {colour_only[0]};
    struct VkRenderPass_T twin = {.device = &d, .attachment_count = 1, .subpass_count = 1,
        .attachments = twin_attachment, .subpasses = twin_subpasses};
    /* Same references, different format: incompatible. */
    VkAttachmentDescription rgba_attachment[1] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT}};
    struct ps5vk_subpass foreign_subpasses[1] = {colour_only[0]};
    struct VkRenderPass_T foreign = {.device = &d, .attachment_count = 1, .subpass_count = 1,
        .attachments = rgba_attachment, .subpasses = foreign_subpasses};
    struct VkFramebuffer_T fb = {.device = &d, .width = 4, .height = 4,
        .attachment_count = 1, .color_attachments = {0}, .color_count = 1, .attachments = {&view},
        .formats = {VK_FORMAT_B8G8R8A8_UNORM}, .samples = {VK_SAMPLE_COUNT_1_BIT},
        .depth_attachment = VK_ATTACHMENT_UNUSED};
    struct VkFramebuffer_T other_fb = fb;
    struct VkPipeline_T graphics = {.device = &d, .graphics = VK_TRUE, .viewport_count = 1,
        .graphics_state = &graphics, .color_format = {VK_FORMAT_B8G8R8A8_UNORM}, .color_attachment_count = 1};
    VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = &pass, .framebuffer = &fb, .renderArea = {.extent = {4,4}}};

    /* A continuation secondary: it inherits the scope and records one draw. */
    VkCommandBufferInheritanceInfo continues = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
        .renderPass = &twin, .framebuffer = &fb};
    VkCommandBufferBeginInfo continue_begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
        .pInheritanceInfo = &continues};
    VkCommandBuffer inside = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    assert(vkBeginCommandBuffer(inside, &continue_begin) == VK_SUCCESS);
    /* The inherited scope is entered at begin, so the draw records exactly as
     * it would in a primary, and a command that may only run OUTSIDE a pass is
     * refused here for the same reason it would be there. */
    assert(inside->render_pass == &twin && inside->render_pass_inherited);
    /* Dynamic state is the secondary's own: a pipeline with dynamic depth bias
     * needs the setter in THIS buffer before its draw, and the draw snapshots
     * exactly what this buffer set. */
    struct VkPipeline_T dynamic_graphics = graphics;
    dynamic_graphics.dynamic_depth_bias = VK_TRUE;
    dynamic_graphics.raster.depth_bias_enable = VK_TRUE;
    vkCmdBindPipeline(inside, VK_PIPELINE_BIND_POINT_GRAPHICS, &dynamic_graphics);
    vkCmdDraw(inside, 3, 1, 0, 0);
    assert(inside->state == PS5VK_INVALID);
    assert(vkBeginCommandBuffer(inside, &continue_begin) == VK_SUCCESS);
    vkCmdBindPipeline(inside, VK_PIPELINE_BIND_POINT_GRAPHICS, &dynamic_graphics);
    vkCmdSetDepthBias(inside, 3.0f, 0.0f, -0.5f);
    vkCmdDraw(inside, 3, 1, 0, 0);
    assert(inside->state == PS5VK_RECORDING && inside->operation_count == 1);
    assert(inside->operations[0].raster.depth_bias_enable &&
           inside->operations[0].raster.depth_bias_constant == 3.0f &&
           inside->operations[0].raster.depth_bias_slope == -0.5f &&
           inside->operations[0].viewport_count == 1);
    /* The submitted recording below uses the shared static pipeline, whose
     * pending count the submission checks own. */
    assert(vkEndCommandBuffer(inside) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(inside, &continue_begin) == VK_SUCCESS);
    vkCmdBindPipeline(inside, VK_PIPELINE_BIND_POINT_GRAPHICS, &graphics);
    vkCmdDraw(inside, 3, 1, 0, 0);
    assert(inside->state == PS5VK_RECORDING && inside->operation_count == 1);
    /* Ending with the INHERITED pass still open is correct: the primary owns
     * it and the secondary has no vkCmdEndRenderPass to give. */
    assert(vkEndCommandBuffer(inside) == VK_SUCCESS);

    /* The primary begins the pass for secondary contents and names the child. */
    VkCommandBuffer host = begun_primary(&d, pool);
    vkCmdBeginRenderPass(host, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(host, 1, &inside);
    vkCmdEndRenderPass(host);
    assert(vkEndCommandBuffer(host) == VK_SUCCESS && host->operation_count == 3);
    VkSubmitInfo pass_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &host};
    assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) == VK_SUCCESS);
    /* ONE segment, not three: the parent's whole BEGIN..END range plus the
     * child's own range, each buffer named once and keeping its identity. */
    assert(d.submission && !d.submission->next && d.submission->count == 2 &&
           !d.submission->frontend_only &&
           d.submission->buffers[0] == host &&
           d.submission->first_operation[0] == 0 &&
           d.submission->operation_count[0] == 3 &&
           d.submission->buffers[1] == inside &&
           d.submission->first_operation[1] == 0 &&
           d.submission->operation_count[1] == 1);
    /* Both are pinned, so neither can be reset or freed while the pass runs,
     * and the pass resources are pinned exactly once. */
    assert(host->pending_count == 1 && inside->pending_count == 1 &&
           pass.pending == 1 && fb.pending == 1 && view.pending == 1 &&
           image->pending == 1 && graphics.pending == 1);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(!host->pending_count && !inside->pending_count && !pass.pending &&
           !fb.pending && !view.pending && !image->pending && !graphics.pending &&
           host->state == PS5VK_EXECUTABLE && inside->state == PS5VK_EXECUTABLE);

    /* The inherited framebuffer is OPTIONAL: the null handle is accepted and
     * the primary supplies the one that executes. */
    VkCommandBufferInheritanceInfo no_fb = continues;
    no_fb.framebuffer = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo no_fb_begin = continue_begin;
    no_fb_begin.pInheritanceInfo = &no_fb;
    VkCommandBuffer floating = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    assert(vkBeginCommandBuffer(floating, &no_fb_begin) == VK_SUCCESS);
    vkCmdBindPipeline(floating, VK_PIPELINE_BIND_POINT_GRAPHICS, &graphics);
    vkCmdDraw(floating, 3, 1, 0, 0);
    assert(vkEndCommandBuffer(floating) == VK_SUCCESS);
    host = begun_primary(&d, pool);
    vkCmdBeginRenderPass(host, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    /* Two children in one call keep their recorded order in the segment. */
    {
        VkCommandBuffer both[2] = {floating, inside};
        vkCmdExecuteCommands(host, 2, both);
    }
    vkCmdEndRenderPass(host);
    assert(vkEndCommandBuffer(host) == VK_SUCCESS);
    pass_submit.pCommandBuffers = &host;
    assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(d.submission && !d.submission->next && d.submission->count == 3 &&
           d.submission->buffers[1] == floating && d.submission->buffers[2] == inside);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);

    /* --- render-pass compatibility is about REFERENCES ------------------- */
    /* Vulkan 1.0 chapter 7.2 compares the corresponding attachment
     * references, not indices, counts or handles. This pass reaches the same
     * colour role through slot 1 and carries an extra attachment that no
     * reference names, with a different format: none of that matters. */
    VkAttachmentDescription shifted_attachments[2] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT},
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT}};
    struct ps5vk_subpass shifted_subpasses[1] = {
        {.color[0] = {.attachment = 1}, .color_count = 1, .depth = {.attachment = VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T shifted = {.device = &d, .attachment_count = 2, .subpass_count = 1,
        .attachments = shifted_attachments, .subpasses = shifted_subpasses};
    assert(ps5vk_render_pass_compatible(&shifted, &pass));
    assert(ps5vk_render_pass_compatible(&pass, &shifted));
    /* Load and store ops and layouts are explicitly excluded from it. */
    VkAttachmentDescription reloaded_attachment[1] = {{
        .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}};
    struct ps5vk_subpass reloaded_subpasses[1] = {
        {.color[0] = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_GENERAL}, .color_count = 1,
         .depth = {.attachment = VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T reloaded = {.device = &d, .attachment_count = 1, .subpass_count = 1,
        .attachments = reloaded_attachment, .subpasses = reloaded_subpasses};
    assert(ps5vk_render_pass_compatible(&reloaded, &pass));
    /* What DOES break it: the referred format, the referred sample count, and
     * a reference that is used on one side and unused on the other. */
    VkAttachmentDescription resampled_attachment[1] = {
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_4_BIT}};
    struct ps5vk_subpass resampled_subpasses[1] = {colour_only[0]};
    struct VkRenderPass_T resampled = {.device = &d, .attachment_count = 1, .subpass_count = 1,
        .attachments = resampled_attachment, .subpasses = resampled_subpasses};
    VkAttachmentDescription depth_attachments[2] = {
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT},
        {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT}};
    struct ps5vk_subpass depth_subpasses[1] = {
        {.color[0] = {.attachment = 0}, .color_count = 1, .depth = {.attachment = 1}}};
    struct VkRenderPass_T with_depth = {.device = &d, .attachment_count = 2, .subpass_count = 1,
        .attachments = depth_attachments, .subpasses = depth_subpasses};
    assert(!ps5vk_render_pass_compatible(&foreign, &pass));
    assert(!ps5vk_render_pass_compatible(&resampled, &pass));
    assert(!ps5vk_render_pass_compatible(&with_depth, &pass));
    assert(!ps5vk_render_pass_compatible(NULL, &pass) &&
           !ps5vk_render_pass_compatible(&pass, NULL));
    /* And the compatible-but-different-shape pass is accepted end to end. */
    {
        VkCommandBufferInheritanceInfo through_slot_one = continues;
        through_slot_one.renderPass = &shifted;
        through_slot_one.framebuffer = VK_NULL_HANDLE;
        VkCommandBufferBeginInfo slot_begin = continue_begin;
        slot_begin.pInheritanceInfo = &through_slot_one;
        VkCommandBuffer elsewhere = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        assert(vkBeginCommandBuffer(elsewhere, &slot_begin) == VK_SUCCESS);
        vkCmdBindPipeline(elsewhere, VK_PIPELINE_BIND_POINT_GRAPHICS, &graphics);
        vkCmdDraw(elsewhere, 3, 1, 0, 0);
        assert(vkEndCommandBuffer(elsewhere) == VK_SUCCESS);
        host = begun_primary(&d, pool);
        vkCmdBeginRenderPass(host, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(host, 1, &elsewhere);
        vkCmdEndRenderPass(host);
        assert(vkEndCommandBuffer(host) == VK_SUCCESS);
        pass_submit.pCommandBuffers = &host;
        assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) == VK_SUCCESS);
        assert(d.submission && d.submission->count == 2);
        assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
        vkFreeCommandBuffers(&d, pool, 1, &elsewhere);
    }

    /* --- a NON-NULL inherited framebuffer of a compatible pass ------------
     * The inherited pass reaches its colour role through slot 1 and carries an
     * extra unreferenced attachment; the inherited framebuffer reaches the
     * same role through slot 0. Compatibility is about roles, so this is a
     * conformant pairing, and the executing framebuffer is still required to
     * be the very same handle. */
    {
        VkCommandBufferInheritanceInfo named_fb = continues;
        named_fb.renderPass = &shifted;
        named_fb.framebuffer = &fb;
        VkCommandBufferBeginInfo named_begin = continue_begin;
        named_begin.pInheritanceInfo = &named_fb;
        VkCommandBuffer paired = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        assert(vkBeginCommandBuffer(paired, &named_begin) == VK_SUCCESS);
        assert(paired->framebuffer == &fb && paired->render_pass == &shifted);
        vkCmdBindPipeline(paired, VK_PIPELINE_BIND_POINT_GRAPHICS, &graphics);
        vkCmdDraw(paired, 3, 1, 0, 0);
        assert(vkEndCommandBuffer(paired) == VK_SUCCESS);
        host = begun_primary(&d, pool);
        vkCmdBeginRenderPass(host, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(host, 1, &paired);
        vkCmdEndRenderPass(host);
        assert(vkEndCommandBuffer(host) == VK_SUCCESS);
        pass_submit.pCommandBuffers = &host;
        assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) == VK_SUCCESS);
        assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
        vkFreeCommandBuffers(&d, pool, 1, &paired);
    }
    /* The role rule is not a licence: a framebuffer whose role disagrees with
     * the pass on format, on sample count, or on used-versus-unused is still
     * refused, and so is one of another device. */
    {
        struct VkFramebuffer_T wrong_format = fb;
        wrong_format.formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
        struct VkFramebuffer_T wrong_samples = fb;
        wrong_samples.samples[0] = VK_SAMPLE_COUNT_4_BIT;
        struct VkFramebuffer_T no_colour = fb;
        no_colour.color_attachments[0] = VK_ATTACHMENT_UNUSED;
        no_colour.color_count = 1;
        VkFramebuffer refused[3] = {&wrong_format, &wrong_samples, &no_colour};
        for (unsigned n = 0; n < 3; ++n) {
            VkCommandBufferInheritanceInfo mismatched = continues;
            mismatched.framebuffer = refused[n];
            VkCommandBufferBeginInfo mismatched_begin = continue_begin;
            mismatched_begin.pInheritanceInfo = &mismatched;
            VkCommandBuffer probe_child = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
            assert(vkBeginCommandBuffer(probe_child, &mismatched_begin) != VK_SUCCESS);
            assert(probe_child->state == PS5VK_INITIAL);
            vkFreeCommandBuffers(&d, pool, 1, &probe_child);
        }
    }

    /* --- an empty render pass is LEGAL to record and unsupported to execute -
     * its load and store ops alone are observable, so refusing it at record
     * time would reject a conformant program. The recording is faithful and
     * SUBMISSION is where this driver admits it cannot execute it. */
    probe = begun_primary(&d, pool);
    vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdEndRenderPass(probe);
    assert(vkEndCommandBuffer(probe) == VK_SUCCESS && probe->operation_count == 2 &&
           probe->state == PS5VK_EXECUTABLE);
    pass_submit.pCommandBuffers = &probe;
    assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) != VK_SUCCESS);
    assert(!d.submission && !probe->pending_count);
    probe = begun_primary(&d, pool);
    vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(probe);
    assert(vkEndCommandBuffer(probe) == VK_SUCCESS && probe->operation_count == 2);
    pass_submit.pCommandBuffers = &probe;
    assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) != VK_SUCCESS);
    assert(!d.submission && !probe->pending_count);
    /* Naming EMPTY secondaries is the same story: the marker is recorded, but
     * nothing it names executes, so the pass carries no work and submission
     * refuses it before any backend sees it. */
    {
        VkCommandBuffer empty_children[2];
        for (unsigned n = 0; n < 2; ++n) {
            empty_children[n] = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
            assert(vkBeginCommandBuffer(empty_children[n], &continue_begin) == VK_SUCCESS);
            assert(vkEndCommandBuffer(empty_children[n]) == VK_SUCCESS);
            assert(empty_children[n]->state == PS5VK_EXECUTABLE &&
                   !empty_children[n]->operation_count);
        }
        probe = begun_primary(&d, pool);
        vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(probe, 1, &empty_children[0]);
        vkCmdEndRenderPass(probe);
        assert(vkEndCommandBuffer(probe) == VK_SUCCESS && probe->operation_count == 3);
        pass_submit.pCommandBuffers = &probe;
        assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) != VK_SUCCESS);
        assert(!d.submission && !probe->pending_count &&
               !empty_children[0]->pending_count);
        /* Several of them are still nothing. */
        probe = begun_primary(&d, pool);
        vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(probe, 2, empty_children);
        vkCmdEndRenderPass(probe);
        assert(vkEndCommandBuffer(probe) == VK_SUCCESS && probe->operation_count == 3);
        pass_submit.pCommandBuffers = &probe;
        assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) != VK_SUCCESS);
        assert(!d.submission && !probe->pending_count);
        /* An empty child ALONGSIDE one that draws is legal and keeps its place
         * in the order: the pass executes work, and naming an empty secondary
         * is a no-op, not an error. */
        VkCommandBuffer ordered[3] = {empty_children[0], inside, empty_children[1]};
        host = begun_primary(&d, pool);
        vkCmdBeginRenderPass(host, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(host, 3, ordered);
        vkCmdEndRenderPass(host);
        assert(vkEndCommandBuffer(host) == VK_SUCCESS);
        pass_submit.pCommandBuffers = &host;
        assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) == VK_SUCCESS);
        assert(d.submission && !d.submission->next && d.submission->count == 4 &&
               d.submission->buffers[0] == host &&
               d.submission->buffers[1] == empty_children[0] &&
               d.submission->buffers[2] == inside &&
               d.submission->buffers[3] == empty_children[1]);
        assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
        /* Submission re-derives the same rule from the immutable record, so a
         * pass whose work disappears after recording is refused there too and
         * never reaches a backend. */
        host = begun_primary(&d, pool);
        vkCmdBeginRenderPass(host, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(host, 1, &inside);
        vkCmdEndRenderPass(host);
        assert(vkEndCommandBuffer(host) == VK_SUCCESS);
        const uint32_t recorded = inside->operation_count;
        inside->operation_count = 0;
        pass_submit.pCommandBuffers = &host;
        assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) != VK_SUCCESS);
        assert(!d.submission && !host->pending_count && !inside->pending_count);
        inside->operation_count = recorded;
        for (unsigned n = 0; n < 2; ++n)
            vkFreeCommandBuffers(&d, pool, 1, &empty_children[n]);
    }

    /* --- refused, each leaving the recording poisoned with no operation --- */
    /* A PRIMARY that claims render-pass continuation is accepted: the flag is
     * ignored there (VUID-vkBeginCommandBuffer-flags-09123) and the pinned
     * upstream render-pass module sets it on its primary buffers. The scope
     * members are not read, so this is an ordinary primary. */
    {
        VkCommandBuffer p2 = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        VkCommandBufferBeginInfo bad_begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT};
        assert(vkBeginCommandBuffer(p2, &bad_begin) == VK_SUCCESS);
        assert(vkEndCommandBuffer(p2) == VK_SUCCESS);
    }
    /* a continuation child OUTSIDE a render pass: its draws have no scope */
    probe = begun_primary(&d, pool);
    vkCmdExecuteCommands(probe, 1, &inside);
    assert(probe->state == PS5VK_INVALID && !probe->operation_count);
    /* a NON-continuation child inside one: it was not recorded for this scope */
    probe = begun_primary(&d, pool);
    vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(probe, 1, &a);
    assert(probe->state == PS5VK_INVALID && probe->operation_count == 1);
    /* an INLINE pass admits no secondaries */
    probe = begun_primary(&d, pool);
    vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdExecuteCommands(probe, 1, &inside);
    assert(probe->state == PS5VK_INVALID && probe->operation_count == 1);
    /* a SECONDARY_COMMAND_BUFFERS pass admits no inline draw of its own */
    probe = begun_primary(&d, pool);
    vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdBindPipeline(probe, VK_PIPELINE_BIND_POINT_GRAPHICS, &graphics);
    vkCmdDraw(probe, 3, 1, 0, 0);
    assert(probe->state == PS5VK_INVALID && probe->operation_count == 1);
    /* an incompatible inherited render pass */
    {
        VkCommandBufferInheritanceInfo wrong = continues;
        /* Self-consistent on its own terms - no framebuffer to disagree with -
         * so it is accepted at begin and refused only where it does not
         * belong: inside a pass of a different attachment format. */
        wrong.renderPass = &foreign; wrong.framebuffer = VK_NULL_HANDLE;
        VkCommandBufferBeginInfo wrong_begin = continue_begin;
        wrong_begin.pInheritanceInfo = &wrong;
        VkCommandBuffer mismatched = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        assert(vkBeginCommandBuffer(mismatched, &wrong_begin) == VK_SUCCESS);
        assert(vkEndCommandBuffer(mismatched) == VK_SUCCESS);
        probe = begun_primary(&d, pool);
        vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(probe, 1, &mismatched);
        assert(probe->state == PS5VK_INVALID && probe->operation_count == 1);
        vkFreeCommandBuffers(&d, pool, 1, &mismatched);
    }
    /* a DIFFERENT framebuffer than the one the pass is executing */
    {
        VkCommandBufferInheritanceInfo elsewhere = continues;
        elsewhere.framebuffer = &other_fb;
        VkCommandBufferBeginInfo elsewhere_begin = continue_begin;
        elsewhere_begin.pInheritanceInfo = &elsewhere;
        VkCommandBuffer stray = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        assert(vkBeginCommandBuffer(stray, &elsewhere_begin) == VK_SUCCESS);
        assert(vkEndCommandBuffer(stray) == VK_SUCCESS);
        probe = begun_primary(&d, pool);
        vkCmdBeginRenderPass(probe, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(probe, 1, &stray);
        assert(probe->state == PS5VK_INVALID && probe->operation_count == 1);
        vkFreeCommandBuffers(&d, pool, 1, &stray);
    }
    /* a subpass index this device does not have */
    {
        VkCommandBufferInheritanceInfo later = continues;
        later.subpass = 1;
        VkCommandBufferBeginInfo later_begin = continue_begin;
        later_begin.pInheritanceInfo = &later;
        VkCommandBuffer beyond = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        assert(vkBeginCommandBuffer(beyond, &later_begin) != VK_SUCCESS);
        vkFreeCommandBuffers(&d, pool, 1, &beyond);
    }
    /* a continuation secondary may not carry work that belongs outside a pass */
    {
        VkCommandBuffer mixed = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        assert(vkBeginCommandBuffer(mixed, &continue_begin) == VK_SUCCESS);
        vkCmdSetEvent(mixed, first, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        assert(mixed->state == PS5VK_INVALID && !mixed->operation_count);
        vkFreeCommandBuffers(&d, pool, 1, &mixed);
    }
    /* and it may not end the pass it inherited */
    {
        VkCommandBuffer closer = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        assert(vkBeginCommandBuffer(closer, &continue_begin) == VK_SUCCESS);
        vkCmdEndRenderPass(closer);
        assert(closer->state == PS5VK_INVALID && !closer->operation_count);
        vkFreeCommandBuffers(&d, pool, 1, &closer);
    }
    /* resetting a named child still invalidates the parent that names it */
    host = begun_primary(&d, pool);
    vkCmdBeginRenderPass(host, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(host, 1, &inside);
    vkCmdEndRenderPass(host);
    assert(vkEndCommandBuffer(host) == VK_SUCCESS);
    assert(vkResetCommandBuffer(inside, 0) == VK_SUCCESS);
    assert(host->state == PS5VK_INVALID);
    pass_submit.pCommandBuffers = &host;
    assert(vkQueueSubmit(&d.queue, 1, &pass_submit, VK_NULL_HANDLE) != VK_SUCCESS);

    vkFreeCommandBuffers(&d, pool, 1, &floating);
    vkFreeCommandBuffers(&d, pool, 1, &inside);
    vkDestroyImage(&d, image, NULL);
    vkFreeMemory(&d, memory, NULL);
    d.graphics_enabled = VK_FALSE; d.graphics_submit_enabled = VK_FALSE;

    vkDestroyEvent(&d, dep_event, NULL);
    vkDestroyEvent(&d, parent_event, NULL);
    vkDestroyEvent(&d, mixed_event, NULL);
    vkDestroyEvent(&d, shared_event, NULL);
    vkDestroyEvent(&d, once_event, NULL);
    vkDestroyEvent(&d, second, NULL);
    vkDestroyEvent(&d, first, NULL);
    vkDestroyCommandPool(&d, pool, NULL);
    puts("Secondary execution: pass (host queue only, no GPU work)");
    return 0;
}

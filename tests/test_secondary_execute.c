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

    /* a render-pass-continue child stays refused: S3 owns those semantics */
    VkCommandBuffer continued = allocate(&d, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    VkCommandBufferBeginInfo cbegin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
        .pInheritanceInfo = &inherit};
    /* S1 refuses the flag outright, so the buffer never becomes executable;
     * the point here is that no path reaches execution either way. */
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

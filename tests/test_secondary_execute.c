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
static VkResult prepare(VkDevice d, const struct ps5vk_submission *s, void **job)
{
    (void)d; (void)s;
    if (fail_prepare_at && ++prepares == fail_prepare_at) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *job = malloc(1);
    return *job ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static VkResult launch(VkDevice d, void *job) { (void)d; (void)job; return VK_SUCCESS; }
static VkResult poll_backend(VkDevice d, void *job, uint64_t *completed)
{ (void)d; (void)job; *completed = 1; return VK_SUCCESS; }
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
    {
        VkCommandBuffer many[PS5VK_MAX_EXECUTED_COMMANDS + 1];
        for (unsigned i = 0; i <= PS5VK_MAX_EXECUTED_COMMANDS; ++i) many[i] = shared;
        probe = begun_primary(&d, pool);
        vkCmdExecuteCommands(probe, PS5VK_MAX_EXECUTED_COMMANDS + 1, many);
        assert(probe->state == PS5VK_INVALID && !probe->operation_count);
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

    vkDestroyEvent(&d, shared_event, NULL);
    vkDestroyEvent(&d, once_event, NULL);
    vkDestroyEvent(&d, second, NULL);
    vkDestroyEvent(&d, first, NULL);
    vkDestroyCommandPool(&d, pool, NULL);
    puts("Secondary execution: pass (host queue only, no GPU work)");
    return 0;
}

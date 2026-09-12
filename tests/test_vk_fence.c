#include "vk_fence.h"
#include <assert.h>
#include <stdio.h>

struct clock_fixture { uint64_t now, signal_at; unsigned polls, pauses; VkResult result; VkFence signal; };
static uint64_t now(void *ctx) { return ((struct clock_fixture *)ctx)->now; }
static void pause_clock(void *ctx, uint64_t remaining)
{
    struct clock_fixture *c = ctx; assert(remaining); ++c->pauses;
    c->now += remaining < 10 ? remaining : 10;
}
static VkResult poll(VkDevice d)
{
    struct clock_fixture *c = d->progress.context; ++c->polls;
    if (c->result != VK_SUCCESS) return c->result;
    /* State-machine fixture only; not a simulated GPU or compute result. */
    if (c->signal && c->now >= c->signal_at) { c->signal->pending_serial = 0; c->signal->signaled = VK_TRUE; }
    return VK_SUCCESS;
}
static VkFence fence(VkDevice d, VkFenceCreateFlags flags)
{
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = flags}; VkFence f;
    assert(vkCreateFence(d, &info, NULL, &f) == VK_SUCCESS); return f;
}
int main(void)
{
    struct clock_fixture clock = {0};
    struct VkDevice_T d = {.progress = {&clock, poll, now, pause_clock}}, other = {0};
    VkFence fences[] = {fence(&d, VK_FENCE_CREATE_SIGNALED_BIT), fence(&d, 0)};
    assert(vkGetFenceStatus(&d, fences[0]) == VK_SUCCESS && !clock.polls);
    assert(vkGetFenceStatus(&d, fences[1]) == VK_NOT_READY && !clock.polls);
    assert(vkWaitForFences(&d, 2, fences, VK_FALSE, 0) == VK_SUCCESS);
    assert(vkWaitForFences(&d, 2, fences, VK_TRUE, 0) == VK_TIMEOUT);
    assert(vkWaitForFences(&d, 2, fences, VK_TRUE, 25) == VK_TIMEOUT && clock.now == 25);
    assert(!clock.polls && clock.pauses == 3);
    fences[1]->pending_serial = 7;
    assert(vkGetFenceStatus(&d, fences[1]) == VK_NOT_READY); /* Poll success is not completion. */
    assert(vkResetFences(&d, 2, fences) != VK_SUCCESS && fences[0]->signaled);
    vkDestroyFence(&d, fences[1], NULL); assert(d.lifetime_errors == 1);
    clock.result = VK_ERROR_DEVICE_LOST;
    assert(vkGetFenceStatus(&d, fences[1]) == VK_ERROR_DEVICE_LOST);
    assert(vkWaitForFences(&d, 2, fences, VK_TRUE, 100) == VK_ERROR_DEVICE_LOST);
    assert(fences[1]->pending_serial == 7 && !fences[1]->signaled);
    clock.result = VK_SUCCESS; clock.signal = fences[1]; clock.signal_at = 45;
    assert(vkWaitForFences(&d, 2, fences, VK_TRUE, UINT64_MAX) == VK_SUCCESS && clock.now == 45);
    assert(vkResetFences(&d, 2, fences) == VK_SUCCESS);
    assert(!fences[0]->signaled && !fences[1]->signaled);
    assert(vkGetFenceStatus(&other, fences[0]) != VK_SUCCESS);
    d.progress.clock_ns = NULL;
    assert(vkWaitForFences(&d, 2, fences, VK_TRUE, 1) == VK_ERROR_UNKNOWN);
    assert(vkWaitForFences(&d, 2, fences, VK_TRUE, 0) == VK_TIMEOUT);
    fences[1]->pending_serial = 9; d.progress.poll = NULL;
    assert(vkGetFenceStatus(&d, fences[1]) == VK_ERROR_UNKNOWN);
    fences[1]->pending_serial = 0;
    vkDestroyFence(&d, fences[0], NULL); vkDestroyFence(&d, fences[1], NULL);
    assert(!d.fences); puts("Fence lifecycle/waits: pass (host state-machine only)");
}

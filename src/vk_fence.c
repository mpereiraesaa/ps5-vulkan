#include "vk_fence.h"

#define INVALID VK_ERROR_UNKNOWN
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice d, const VkFenceCreateInfo *info,
    const VkAllocationCallbacks *a, VkFence *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_FENCE_CREATE_INFO || info->pNext ||
        (info->flags & ~VK_FENCE_CREATE_SIGNALED_BIT)) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkFence f = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, a,
        sizeof(*f), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    f->device = d; f->allocator = saved; f->custom_allocator = custom;
    f->signaled = !!(info->flags & VK_FENCE_CREATE_SIGNALED_BIT);
    f->next = d->fences; d->fences = f; *out = f;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !f || f->device != d) return;
    if (f->pending_serial) { ++d->lifetime_errors; return; }
    VkFence *link = &d->fences;
    while (*link && *link != f) link = &(*link)->next;
    if (!*link) return;
    *link = f->next;
    VkAllocationCallbacks saved = f->allocator; VkBool32 custom = f->custom_allocator;
    ps5vk_object_free(f, &saved, custom);
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice d, uint32_t count, const VkFence *fences)
{
    if (!d || !count || !fences) return INVALID;
    for (uint32_t j = 0; j < count; ++j)
        if (!fences[j] || fences[j]->device != d || fences[j]->pending_serial) return INVALID;
    for (uint32_t j = 0; j < count; ++j) fences[j]->signaled = VK_FALSE;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice d, VkFence f)
{
    if (!d || !f || f->device != d) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    if (f->pending_serial) {
        if (!d->progress.poll) return INVALID;
        VkResult result = d->progress.poll(d);
        if (result != VK_SUCCESS) return result;
    }
    /* Poll success means only the poll ran. It must separately retire this
     * exact submission/fence after GPU completion and cache visibility. */
    return f->signaled && !f->pending_serial ? VK_SUCCESS : VK_NOT_READY;
}
static int satisfied(uint32_t count, const VkFence *fences, VkBool32 all)
{
    unsigned ready = 0;
    for (uint32_t j = 0; j < count; ++j) ready += fences[j]->signaled && !fences[j]->pending_serial;
    return all ? ready == count : ready != 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d, uint32_t count, const VkFence *fences,
                                             VkBool32 all, uint64_t timeout)
{
    if (!d || !count || !fences) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    for (uint32_t j = 0; j < count; ++j) if (!fences[j] || fences[j]->device != d) return INVALID;
    if (satisfied(count, fences, all)) return VK_SUCCESS;
    /* Timeout-zero remains a nonblocking poll and does not require a clock. */
    if (timeout && (!d->progress.clock_ns || !d->progress.pause)) return INVALID;
    uint64_t start = timeout ? d->progress.clock_ns(d->progress.context) : 0;
    for (;;) {
        VkBool32 pending = VK_FALSE;
        for (uint32_t j = 0; j < count; ++j) pending |= fences[j]->pending_serial != 0;
        if (pending) {
            if (!d->progress.poll) return INVALID;
            VkResult result = d->progress.poll(d);
            if (result != VK_SUCCESS) return result;
        }
        if (satisfied(count, fences, all)) return VK_SUCCESS;
        if (!timeout) return VK_TIMEOUT;
        uint64_t elapsed = d->progress.clock_ns(d->progress.context) - start;
        if (timeout != UINT64_MAX && elapsed >= timeout) return VK_TIMEOUT;
        uint64_t remaining = timeout == UINT64_MAX ? UINT64_MAX : timeout - elapsed;
        d->progress.pause(d->progress.context, remaining);
    }
}

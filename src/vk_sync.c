#include "vk_sync.h"

#define INVALID VK_ERROR_UNKNOWN

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice d,
    const VkSemaphoreCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkSemaphore *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO ||
        info->pNext || info->flags) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkSemaphore semaphore = ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, allocator,
        sizeof(*semaphore), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!semaphore) return VK_ERROR_OUT_OF_HOST_MEMORY;
    semaphore->device = d; semaphore->allocator = saved;
    semaphore->custom_allocator = custom;
    semaphore->next = d->semaphores; d->semaphores = semaphore;
    *out = semaphore;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice d, VkSemaphore semaphore,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!d || !semaphore || semaphore->device != d) return;
    if (semaphore->pending) { ++d->lifetime_errors; return; }
    VkSemaphore *link = &d->semaphores;
    while (*link && *link != semaphore) link = &(*link)->next;
    if (!*link) return;
    *link = semaphore->next;
    VkAllocationCallbacks saved = semaphore->allocator;
    VkBool32 custom = semaphore->custom_allocator;
    ps5vk_object_free(semaphore, &saved, custom);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(VkDevice d,
    const VkEventCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkEvent *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_EVENT_CREATE_INFO ||
        info->pNext || info->flags) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkEvent event = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL,
        allocator, sizeof(*event), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
        &saved, &custom);
    if (!event) return VK_ERROR_OUT_OF_HOST_MEMORY;
    event->device = d; event->allocator = saved;
    event->custom_allocator = custom;
    event->next = d->events; d->events = event;
    *out = event;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(VkDevice d, VkEvent event,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!d || !event || event->device != d) return;
    if (event->pending) { ++d->lifetime_errors; return; }
    if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_EVENT, event)) {
        ++d->lifetime_errors; return;
    }
    VkEvent *link = &d->events;
    while (*link && *link != event) link = &(*link)->next;
    if (!*link) return;
    *link = event->next;
    VkAllocationCallbacks saved = event->allocator;
    VkBool32 custom = event->custom_allocator;
    ps5vk_object_free(event, &saved, custom);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice d, VkEvent event)
{
    if (!d || !event || event->device != d) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    return event->host_signaled || event->device_signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice d, VkEvent event)
{
    if (!d || !event || event->device != d) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    /* VUID-vkSetEvent-event-09543: a host set cannot rescue an event that is
     * already waited on by a pending command buffer.  Track waits separately
     * from general pending ownership so pending device SET/RESET operations do
     * not spuriously prohibit a host transition. */
    if (event->pending_waits) return INVALID;
    event->host_signaled = VK_TRUE;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice d, VkEvent event)
{
    if (!d || !event || event->device != d) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    /* This bounded single-queue implementation accepts host reset only when
     * no pending device wait needs an execution dependency against it. */
    if (event->pending_waits) return INVALID;
    event->host_signaled = VK_FALSE;
    event->device_signaled = VK_FALSE;
    return VK_SUCCESS;
}

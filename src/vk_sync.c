#include "vk_sync.h"
#include "vk_queue.h"

#define INVALID VK_ERROR_UNKNOWN

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice d,
    const VkSemaphoreCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkSemaphore *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO ||
        info->flags) return INVALID;
    VkSemaphoreType type = VK_SEMAPHORE_TYPE_BINARY;
    uint64_t initial = 0;
    VkBool32 saw_type = VK_FALSE;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        /* The type structure belongs to VK_KHR_timeline_semaphore; anything
         * else, or a second copy, is refused before any state is created. */
        if (next->sType != VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO ||
            saw_type || !d->timeline_extension_enabled) return INVALID;
        saw_type = VK_TRUE;
        const VkSemaphoreTypeCreateInfo *type_info =
            (const VkSemaphoreTypeCreateInfo *)next;
        if (type_info->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
            /* VUID-VkSemaphoreTypeCreateInfo-timelineSemaphore-03252 */
            if (!(d->enabled_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE))
                return INVALID;
        } else if (type_info->semaphoreType != VK_SEMAPHORE_TYPE_BINARY ||
                   type_info->initialValue) {
            /* VUID-VkSemaphoreTypeCreateInfo-semaphoreType-03279 */
            return INVALID;
        }
        type = type_info->semaphoreType;
        initial = type_info->initialValue;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkSemaphore semaphore = ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, allocator,
        sizeof(*semaphore), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!semaphore) return VK_ERROR_OUT_OF_HOST_MEMORY;
    semaphore->device = d; semaphore->allocator = saved;
    semaphore->custom_allocator = custom;
    semaphore->type = type;
    semaphore->value = initial;
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

static int timeline_semaphore(VkDevice d, VkSemaphore semaphore)
{
    return semaphore && semaphore->device == d &&
        semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE;
}

/* Let queued work that a payload now satisfies make progress. The poll hook
 * belongs to the queue and takes the device lock itself. */
static VkResult make_progress(VkDevice d)
{ return d->progress.poll ? d->progress.poll(d) : VK_SUCCESS; }

VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValueKHR(VkDevice d,
    VkSemaphore semaphore, uint64_t *value)
{
    if (!d || !value || !timeline_semaphore(d, semaphore)) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    /* Retire whatever the backend has completed so a caller that only polls
     * the counter still observes finished work. */
    VkResult result = make_progress(d);
    if (result != VK_SUCCESS) return result;
    ps5vk_device_lock(d);
    *value = semaphore->value;
    ps5vk_device_unlock(d);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR(VkDevice d,
    const VkSemaphoreWaitInfo *info, uint64_t timeout)
{
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO ||
        info->pNext || (info->flags & ~(VkSemaphoreWaitFlags)VK_SEMAPHORE_WAIT_ANY_BIT) ||
        !info->semaphoreCount || !info->pSemaphores || !info->pValues)
        return INVALID;
    for (uint32_t j = 0; j < info->semaphoreCount; ++j)
        if (!timeline_semaphore(d, info->pSemaphores[j])) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    const VkBool32 any = !!(info->flags & VK_SEMAPHORE_WAIT_ANY_BIT);
    /* Timeout zero is a nonblocking poll and does not require a clock. */
    if (timeout && (!d->progress.clock_ns || !d->progress.pause)) return INVALID;
    const uint64_t start = timeout ? d->progress.clock_ns(d->progress.context) : 0;
    for (;;) {
        VkResult result = make_progress(d);
        if (result != VK_SUCCESS) return result;
        uint32_t ready = 0;
        ps5vk_device_lock(d);
        for (uint32_t j = 0; j < info->semaphoreCount; ++j)
            ready += info->pSemaphores[j]->value >= info->pValues[j];
        ps5vk_device_unlock(d);
        if (any ? ready != 0 : ready == info->semaphoreCount) return VK_SUCCESS;
        if (!timeout) return VK_TIMEOUT;
        const uint64_t elapsed = d->progress.clock_ns(d->progress.context) - start;
        if (timeout != UINT64_MAX && elapsed >= timeout) return VK_TIMEOUT;
        d->progress.pause(d->progress.context,
            timeout == UINT64_MAX ? UINT64_MAX : timeout - elapsed);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphoreKHR(VkDevice d,
    const VkSemaphoreSignalInfo *info)
{
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO ||
        info->pNext || !timeline_semaphore(d, info->semaphore)) return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    VkSemaphore semaphore = info->semaphore;
    VkResult result = VK_SUCCESS;
    ps5vk_device_lock(d);
    /* VUID-VkSemaphoreSignalInfo-value-03258: strictly greater than the
     * current payload, so the payload is monotonic. */
    if (info->value <= semaphore->value) result = INVALID;
    /* VUID-VkSemaphoreSignalInfo-value-03259: strictly smaller than every
     * signal operation still queued, so it cannot overtake one. */
    for (const struct ps5vk_submission *s = d->submission;
         s && result == VK_SUCCESS; s = s->next)
        for (uint32_t j = 0; j < s->signal_count; ++j)
            if (s->signals[j] == semaphore && s->signal_values[j] <= info->value)
                result = INVALID;
    if (result == VK_SUCCESS) semaphore->value = info->value;
    ps5vk_device_unlock(d);
    /* The host signal only publishes the payload. Queued work waiting on it
     * is started by the next progress call (a wait, a poll, a counter query or
     * a submission), so this call never blocks on GPU work. */
    return result;
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

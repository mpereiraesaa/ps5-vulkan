#include "vk_sync.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned allocations, frees;
static void *VKAPI_PTR allocate(void *user, size_t size, size_t alignment,
    VkSystemAllocationScope scope)
{
    (void)user; (void)alignment; (void)scope; ++allocations;
    return calloc(1, size);
}
static void *VKAPI_PTR reallocate(void *user, void *original, size_t size,
    size_t alignment, VkSystemAllocationScope scope)
{
    (void)user; (void)alignment; (void)scope;
    return realloc(original, size);
}
static void VKAPI_PTR release(void *user, void *memory)
{ (void)user; ++frees; free(memory); }

int main(void)
{
    struct VkDevice_T d = {0}, other = {0};
    VkAllocationCallbacks allocator = {
        .pfnAllocation = allocate, .pfnReallocation = reallocate,
        .pfnFree = release,
    };
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkEventCreateInfo event_info = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkSemaphore semaphore = (VkSemaphore)(uintptr_t)1;
    VkEvent event = (VkEvent)(uintptr_t)1;

    assert(vkCreateSemaphore(NULL, &semaphore_info, NULL, &semaphore) != VK_SUCCESS);
    assert(!semaphore);
    semaphore_info.flags = 1;
    assert(vkCreateSemaphore(&d, &semaphore_info, NULL, &semaphore) != VK_SUCCESS);
    semaphore_info.flags = 0;
    assert(vkCreateSemaphore(&d, &semaphore_info, &allocator, &semaphore) == VK_SUCCESS);
    assert(semaphore && semaphore->device == &d && d.semaphores == semaphore);
    vkDestroySemaphore(&other, semaphore, NULL);
    assert(d.semaphores == semaphore);
    semaphore->pending = 1;
    vkDestroySemaphore(&d, semaphore, NULL);
    assert(d.semaphores == semaphore && d.lifetime_errors == 1);
    semaphore->pending = 0;
    vkDestroySemaphore(&d, semaphore, NULL);
    assert(!d.semaphores && allocations == 1 && frees == 1);

    assert(vkCreateEvent(&d, &event_info, &allocator, &event) == VK_SUCCESS);
    assert(event && vkGetEventStatus(&d, event) == VK_EVENT_RESET);
    assert(vkSetEvent(&d, event) == VK_SUCCESS);
    assert(vkGetEventStatus(&d, event) == VK_EVENT_SET);
    assert(vkResetEvent(&d, event) == VK_SUCCESS);
    assert(vkGetEventStatus(&d, event) == VK_EVENT_RESET);
    event->device_signaled = VK_TRUE;
    assert(vkGetEventStatus(&d, event) == VK_EVENT_SET);
    event->pending = 1;
    assert(vkSetEvent(&d, event) != VK_SUCCESS);
    assert(vkResetEvent(&d, event) != VK_SUCCESS);
    vkDestroyEvent(&d, event, NULL);
    assert(d.events == event && d.lifetime_errors == 2);
    event->pending = 0;
    d.lost = VK_TRUE;
    assert(vkGetEventStatus(&d, event) == VK_ERROR_DEVICE_LOST);
    assert(vkSetEvent(&d, event) == VK_ERROR_DEVICE_LOST);
    d.lost = VK_FALSE;
    vkDestroyEvent(&d, event, NULL);
    assert(!d.events && allocations == 2 && frees == 2);

    event_info.pNext = &event_info;
    assert(vkCreateEvent(&d, &event_info, NULL, &event) != VK_SUCCESS && !event);
    return 0;
}

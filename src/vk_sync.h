#ifndef PS5VK_SYNC_H
#define PS5VK_SYNC_H

#include "vk_internal.h"

struct VkSemaphore_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    /* Binary payload as of the last retired queue operation. */
    VkBool32 signaled;
    uint32_t pending;
    struct VkSemaphore_T *next;
    VkSemaphoreType type;
    /* Timeline payload, guarded by the device queue lock. It advances only
     * when vkSignalSemaphoreKHR executes or when a submission retires after
     * the backend reported its exact completion; registering a signal never
     * advances it, and it never decreases. */
    uint64_t value;
};

struct VkEvent_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkBool32 host_signaled;
    VkBool32 device_signaled;
    uint32_t pending;
    uint32_t pending_waits;
    struct VkEvent_T *next;
};

#endif

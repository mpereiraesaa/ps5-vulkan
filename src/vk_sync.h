#ifndef PS5VK_SYNC_H
#define PS5VK_SYNC_H

#include "vk_internal.h"

struct VkSemaphore_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkBool32 signaled;
    uint32_t pending;
    struct VkSemaphore_T *next;
};

struct VkEvent_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkBool32 host_signaled;
    VkBool32 device_signaled;
    uint32_t pending;
    struct VkEvent_T *next;
};

#endif

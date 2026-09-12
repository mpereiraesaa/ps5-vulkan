#ifndef PS5VK_FENCE_H
#define PS5VK_FENCE_H
#include "vk_internal.h"
struct VkFence_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkBool32 signaled;
    uint64_t pending_serial;
    struct VkFence_T *next;
};
#endif

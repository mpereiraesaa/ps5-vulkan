#ifndef PS5VK_SAMPLER_H
#define PS5VK_SAMPLER_H
#include "vk_internal.h"
struct VkSampler_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    uint32_t words[4];
};
#endif

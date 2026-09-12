#ifndef PS5VK_DESCRIPTOR_H
#define PS5VK_DESCRIPTOR_H
#include "vk_internal.h"

/* Implementation bounds, not a claim of Vulkan minimum-limit conformance. */
enum { PS5VK_MAX_BINDINGS = 32, PS5VK_MAX_DESCRIPTORS = 128, PS5VK_MAX_SETS = 4 };
struct ps5vk_binding {
    uint32_t count, first;
    VkShaderStageFlags stages;
};
struct ps5vk_set_signature {
    struct ps5vk_binding binding[PS5VK_MAX_BINDINGS];
    /* Zero preserves the existing storage-buffer contract in host fixtures. */
    VkBool32 combined_image[PS5VK_MAX_BINDINGS];
    uint32_t count;
};
struct VkDescriptorSetLayout_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    struct ps5vk_set_signature signature;
};
struct VkDescriptorSet_T {
    VkDescriptorPool pool;
    struct VkDescriptorSet_T *next;
    struct ps5vk_set_signature signature;
    VkDescriptorBufferInfo buffers[PS5VK_MAX_DESCRIPTORS];
    VkDescriptorImageInfo images[PS5VK_MAX_DESCRIPTORS];
    VkImage image_resources[PS5VK_MAX_DESCRIPTORS];
    VkBool32 defined[PS5VK_MAX_DESCRIPTORS];
    uint64_t generation;
    unsigned pending;
};
struct VkDescriptorPool_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkDescriptorPoolCreateFlags flags;
    uint32_t max_sets, used_sets;
    uint64_t capacity, used;
    uint64_t image_capacity, image_used;
    VkDescriptorSet sets;
};
struct VkPipelineLayout_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    uint32_t set_count;
    struct ps5vk_set_signature sets[PS5VK_MAX_SETS];
};
#endif

#ifndef PS5VK_DESCRIPTOR_H
#define PS5VK_DESCRIPTOR_H
#include "vk_internal.h"

/* Implementation bounds, not a claim of Vulkan minimum-limit conformance. */
enum { PS5VK_MAX_BINDINGS = 32, PS5VK_MAX_DESCRIPTORS = 128, PS5VK_MAX_SETS = 4 };
enum { PS5VK_MAX_PUSH_CONSTANT_BYTES = 256, PS5VK_MAX_PUSH_CONSTANT_DWORDS = 64 };
struct VkBufferView_T {
    VkDevice device;
    VkBuffer buffer;
    VkFormat format;
    VkDeviceSize offset, range;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    struct VkBufferView_T *next;
};
struct ps5vk_binding {
    uint32_t count, first;
    VkShaderStageFlags stages;
};
struct ps5vk_set_signature {
    struct ps5vk_binding binding[PS5VK_MAX_BINDINGS];
    VkDescriptorType type[PS5VK_MAX_BINDINGS];
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
    VkBufferView texel_views[PS5VK_MAX_DESCRIPTORS];
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
    uint64_t storage_capacity, storage_used;
    uint64_t uniform_capacity, uniform_used;
    uint64_t dynamic_storage_capacity, dynamic_storage_used;
    uint64_t dynamic_uniform_capacity, dynamic_uniform_used;
    uint64_t texel_capacity, texel_used;
    uint64_t image_capacity, image_used;
    /* Input attachments are image-view-only descriptors with their own pool
     * accounting: a pool that sized itself for sampled images does not thereby
     * hold input attachments, exactly as the other roles are kept apart. */
    uint64_t input_capacity, input_used;
    VkDescriptorSet sets;
};

static inline VkBool32 ps5vk_dynamic_descriptor_type(VkDescriptorType type)
{
    return type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
           type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

static inline VkDescriptorType ps5vk_base_buffer_descriptor_type(VkDescriptorType type)
{
    return type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ?
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
        type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ?
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : type;
}
struct VkPipelineLayout_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    uint32_t set_count;
    struct ps5vk_set_signature sets[PS5VK_MAX_SETS];
    uint32_t push_constant_size;
    VkShaderStageFlags push_constant_stages[PS5VK_MAX_PUSH_CONSTANT_DWORDS];
};
#endif

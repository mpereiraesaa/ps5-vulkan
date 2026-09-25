#ifndef PS5VK_DESCRIPTOR_H
#define PS5VK_DESCRIPTOR_H
#include "vk_internal.h"

/* Implementation bounds, not a claim of Vulkan minimum-limit conformance. */
enum { PS5VK_MAX_BINDINGS = 32, PS5VK_MAX_DESCRIPTORS = 128, PS5VK_MAX_SETS = 4 };
enum { PS5VK_MAX_PUSH_CONSTANT_BYTES = 256, PS5VK_MAX_PUSH_CONSTANT_DWORDS = 64 };
/* Inline uniform blocks (DXVK262-T12). These are the bounds the descriptor
 * model below enforces, and the values a Vulkan 1.3 report would carry: each
 * equals or exceeds the core minimum (256 bytes, 4 blocks, 256 bytes total).
 * Update-after-bind layouts are refused, so the two update-after-bind limits
 * equal the plain ones. The descriptor model does not make the feature
 * consumable: no shader path encodes an inline block yet, and every
 * consumer refuses the descriptor type explicitly. */
enum {
    PS5VK_MAX_INLINE_UNIFORM_BLOCK_BYTES = 256,
    PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_SET = 4,
    PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_STAGE = 4,
    PS5VK_MAX_INLINE_UNIFORM_SET_BYTES =
        PS5VK_MAX_INLINE_UNIFORM_BLOCK_BYTES * PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_SET,
    PS5VK_MAX_INLINE_UNIFORM_TOTAL_BYTES = PS5VK_MAX_INLINE_UNIFORM_SET_BYTES
};
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
/* An inline uniform block occupies ONE descriptor slot of the signature (the
 * slot a future buffer record would address) and bytes[] of the set's own
 * inline storage. bytes[b] is zero for every other binding. */
struct ps5vk_inline_uniform_layout {
    uint32_t offset[PS5VK_MAX_BINDINGS], bytes[PS5VK_MAX_BINDINGS];
    uint32_t total_bytes, blocks;
};
struct VkDescriptorSetLayout_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    struct ps5vk_set_signature signature;
    struct ps5vk_inline_uniform_layout inline_uniform;
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
    struct ps5vk_inline_uniform_layout inline_uniform;
    uint8_t inline_data[PS5VK_MAX_INLINE_UNIFORM_SET_BYTES];
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
    uint64_t storage_image_capacity, storage_image_used;
    /* Input attachments are image-view-only descriptors with their own pool
     * accounting: a pool that sized itself for sampled images does not thereby
     * hold input attachments, exactly as the other roles are kept apart. */
    uint64_t input_capacity, input_used;
    /* The separate-sampler, sampled-image and storage-texel types DXVK sizes
     * every pool with. Each has its own shader-table record (S#, T#, V#);
     * the compute path compiles and encodes them, graphics still refuses. */
    uint64_t sampler_capacity, sampler_used;
    uint64_t sampled_image_capacity, sampled_image_used;
    uint64_t storage_texel_capacity, storage_texel_used;
    /* Inline uniform blocks are accounted twice, as Vulkan sizes them: bytes
     * from VkDescriptorPoolSize and bindings from
     * VkDescriptorPoolInlineUniformBlockCreateInfo (zero when absent). */
    uint64_t inline_bytes_capacity, inline_bytes_used;
    uint64_t inline_bindings_capacity, inline_bindings_used;
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
/* A DESCRIPTOR_SET update template: validated entries plus the layout
 * signature a compatible set must carry. */
struct VkDescriptorUpdateTemplate_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    struct ps5vk_set_signature signature;
    uint32_t entry_count;
    VkDescriptorUpdateTemplateEntry entries[];
};
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

#ifndef PS5VK_GRAPHICS_PROGRAM_H
#define PS5VK_GRAPHICS_PROGRAM_H
#include <vulkan/vulkan_core.h>
#include <stddef.h>
#include "vk_descriptor.h"
struct ps5vk_graphics_module_key {
    const uint32_t *words;
    size_t word_count;
    const char *entry;
};
/* Offline compilation identity includes the complete vertex-input layout.
 * Description arrays are borrowed for lookup; library records must own them.
 * Descriptor signatures are borrowed during lookup and compared by value.
 * Raster/depth/viewport dynamic state is not part of this shader compile key. */
struct ps5vk_graphics_key {
    struct ps5vk_graphics_module_key vertex, fragment;
    VkPrimitiveTopology topology;
    VkFormat color_format;
    VkSampleCountFlagBits samples;
    VkColorComponentFlags color_write_mask;
    VkBool32 blend_enable;
    uint32_t vertex_binding_count, vertex_attribute_count, descriptor_set_count;
    const VkVertexInputBindingDescription *vertex_bindings;
    const VkVertexInputAttributeDescription *vertex_attributes;
    const struct ps5vk_set_signature *descriptor_sets;
};
struct ps5vk_graphics_program {
    struct ps5vk_graphics_key key;
    const void *backend_data;
};
struct ps5vk_graphics_library {
    const struct ps5vk_graphics_program *programs;
    size_t count;
};
VkResult ps5vk_graphics_resolve(const struct ps5vk_graphics_library *,
    const struct ps5vk_graphics_key *, const struct ps5vk_graphics_program **);
#endif

#ifndef PS5VK_GRAPHICS_PROGRAM_H
#define PS5VK_GRAPHICS_PROGRAM_H
#include <vulkan/vulkan_core.h>
#include <stddef.h>
#include "vk_descriptor.h"
struct ps5vk_graphics_specialization {
    uint32_t constant_id, size;
    uint8_t data[8];
};
struct ps5vk_graphics_module_key {
    const uint32_t *words;
    size_t word_count;
    const char *entry;
    uint32_t specialization_count;
    struct ps5vk_graphics_specialization specializations[64];
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
    uint32_t push_constant_size;
    VkShaderStageFlags push_constant_stages[PS5VK_MAX_PUSH_CONSTANT_DWORDS];
};
struct ps5vk_graphics_program {
    struct ps5vk_graphics_key key;
    const void *backend_data;
};
struct ps5vk_graphics_library {
    const struct ps5vk_graphics_program *programs;
    size_t count;
};
/* Topologies this profile accepts, and the GFX1013 primitive-type value the AGC
 * linker programs for each of them. The pinned compiler's primitive enum
 * (third_party/psbc-reference libpsbc/psbc_compile.c ps5_last_provoking_vertex)
 * and the pinned register header (amdgfxregs.h V_030908_DI_PT_TRILIST = 4,
 * V_030908_DI_PT_TRISTRIP = 6) agree on both values, so the shader compile and
 * the linked pipeline cannot disagree about the primitive they were built for.
 * Every other topology stays fail-closed. */
#define PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST 4u
#define PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP 6u
static inline int ps5vk_agc_primitive_type(VkPrimitiveTopology topology,uint32_t *out)
{
    if(!out)return -1;
    switch(topology) {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
        *out=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST;return 0;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        *out=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP;return 0;
    default:
        return -1;
    }
}
VkResult ps5vk_graphics_resolve(const struct ps5vk_graphics_library *,
    const struct ps5vk_graphics_key *, const struct ps5vk_graphics_program **);
#endif

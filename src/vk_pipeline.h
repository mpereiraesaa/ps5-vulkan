#ifndef PS5VK_PIPELINE_H
#define PS5VK_PIPELINE_H
#include "vk_descriptor.h"

struct ps5vk_program_descriptor {
    uint32_t set, binding, element, table_dword;
};
struct ps5vk_compiled_program {
    const uint32_t *spirv, *code;
    size_t spirv_words, code_words;
    const char *entry;
    uint32_t gfx, local_size[3], wave_size, vgprs, sgprs, float_mode;
    uint32_t ieee_mode, mem_ordered, user_sgprs, wgp_mode;
    uint32_t tg_size, tgid[3], tidig_components;
    uint32_t descriptor_count;
    struct ps5vk_program_descriptor descriptors[PS5VK_MAX_BINDINGS];
};
struct ps5vk_program_library {
    const struct ps5vk_compiled_program *programs;
    size_t count;
};
/* Offline-compiled library lookup is byte-exact and has no fallback program.
 * It is NOT native runtime SPIR-V compilation; unknown modules fail explicitly. */
VkResult ps5vk_program_resolve(void *library, const uint32_t *words, size_t count,
                             const char *entry, const struct ps5vk_compiled_program **out);

struct VkShaderModule_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    size_t word_count;
    uint32_t words[];
};
VkBool32 ps5vk_shader_entry(VkShaderModule, VkShaderStageFlagBits, const char *, uint32_t *id);
struct VkPipeline_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    uint32_t set_count;
    struct ps5vk_set_signature sets[PS5VK_MAX_SETS];
    struct ps5vk_compiled_program program;
    void *cache_entry;
    VkBool32 graphics;
    void *graphics_state;
    void (*graphics_release)(VkDevice, void *);
    VkViewport viewport;
    VkRect2D scissor;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkBool32 depth_test, depth_write;
    VkCompareOp depth_compare;
    VkFormat color_format, depth_format;
    uint32_t vertex_binding_count, vertex_attribute_count;
    VkVertexInputBindingDescription vertex_binding;
    VkVertexInputAttributeDescription vertex_attributes[32];
    uint32_t code[];
};
#endif

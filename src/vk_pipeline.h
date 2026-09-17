#ifndef PS5VK_PIPELINE_H
#define PS5VK_PIPELINE_H
#include "vk_descriptor.h"

struct ps5vk_program_descriptor {
    uint32_t set, binding, element, table_dword;
    VkDescriptorType type;
};
struct ps5vk_compiled_program {
    const uint32_t *spirv, *code;
    size_t spirv_words, code_words;
    const char *entry;
    uint32_t gfx, local_size[3], wave_size, vgprs, sgprs, float_mode;
    uint32_t ieee_mode, mem_ordered, user_sgprs, wgp_mode;
    uint32_t tg_size, tgid[3], tidig_components;
    /* Pinned PSBC compute ABI: optional inline grid dimensions at s3..s5.
     * LDS_SIZE is in the compiler's 512-byte allocation units. */
    uint32_t grid_size_sgpr, lds_size;
    uint32_t push_constant_size, push_constant_sgpr;
    uint32_t descriptor_set_mask;
    uint32_t descriptor_set_sgpr[PS5VK_MAX_SETS];
    uint32_t descriptor_count;
    struct ps5vk_program_descriptor descriptors[PS5VK_MAX_DESCRIPTORS];
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
/* Rasterization state a draw executes with. A pipeline holds its static values;
 * a recorded draw holds a BY-VALUE copy resolved at record time (the pipeline's
 * value, or the command buffer's current dynamic value where the pipeline
 * declared that state dynamic), so a later setter or a later pipeline cannot
 * reach a draw that was already recorded. The native encoder consumes exactly
 * this snapshot. depth_bias_enable is kept apart from the factors: Vulkan
 * ignores the factors while the enable is false, and an enabled zero bias is a
 * different state from a disabled one only in the enable bits it programs. */
struct ps5vk_raster_state {
    VkBool32 depth_bias_enable;
    float depth_bias_constant, depth_bias_clamp, depth_bias_slope;
    /* depthClampEnable: near/far clipping is replaced by clamping z_f to the
     * viewport's [min(n,f), max(n,f)]. Static in this profile. */
    VkBool32 depth_clamp;
    /* VK_POLYGON_MODE_FILL, _LINE or _POINT (fillModeNonSolid for the last
     * two). Static in this profile. */
    VkPolygonMode polygon_mode;
};
struct VkPipeline_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    uint32_t set_count;
    struct ps5vk_set_signature sets[PS5VK_MAX_SETS];
    uint32_t push_constant_size;
    VkShaderStageFlags push_constant_stages[PS5VK_MAX_PUSH_CONSTANT_DWORDS];
    struct ps5vk_compiled_program program;
    void *cache_entry;
    VkBool32 graphics;
    /* The subpass this graphics pipeline was created against. A pipeline is
     * bound to ONE subpass of one render pass, so a draw recorded in a
     * different subpass is refused rather than executed with the state of the
     * wrong one. Meaningless for a compute pipeline. */
    uint32_t subpass;
    void *graphics_state;
    void (*graphics_release)(VkDevice, void *);
    VkViewport viewport;
    VkRect2D scissor;
    VkBool32 dynamic_viewport, dynamic_scissor;
    /* Static rasterization state, and whether VK_DYNAMIC_STATE_DEPTH_BIAS was
     * declared: when it was, the three factors here are unused and a draw
     * takes them from the command buffer instead. */
    struct ps5vk_raster_state raster;
    VkBool32 dynamic_depth_bias;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkBool32 depth_test, depth_write;
    VkCompareOp depth_compare;
    VkFormat color_format, depth_format;
    uint32_t vertex_binding_count, vertex_attribute_count;
    union {
        VkVertexInputBindingDescription vertex_binding; /* first description */
        VkVertexInputBindingDescription vertex_bindings[16];
    };
    VkVertexInputAttributeDescription vertex_attributes[32];
    uint32_t code[];
};
#endif

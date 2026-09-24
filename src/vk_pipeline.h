#ifndef PS5VK_PIPELINE_H
#define PS5VK_PIPELINE_H
#include "vk_descriptor.h"
#include "graphics_limits.h"
#include "color_attachment_contract.h"

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
/* Viewport/scissor array capacity of the pipeline, command-buffer and draw
 * snapshot storage, and the maxViewports the multiViewport feature would
 * report (the Vulkan floor for that feature is 16). The native encoder has one
 * register bank per index (PA_CL_VPORT_*, PA_SC_VPORT_ZMIN/ZMAX,
 * PA_SC_VPORT_SCISSOR) for exactly this many; the reported limit stays at one
 * until the feature is promoted. */
enum { PS5VK_MAX_VIEWPORTS = PS5VK_MULTI_VIEWPORT_COUNT };
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
    /* Stencil test, resolved per draw: the pipeline's static state with the
     * command buffer's dynamic compare mask, write mask and reference folded
     * in when the pipeline declared them dynamic. Only a depth/stencil
     * attachment with a stencil aspect enables it. */
    VkBool32 stencil_test;
    VkStencilOpState stencil_front, stencil_back;
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
    VkBool32 dispatch_base_enabled;
    VkBool32 graphics_usage_known;
    uint32_t graphics_used_set_mask;
    /* The subpass this graphics pipeline was created against. A pipeline is
     * bound to ONE subpass of one render pass, so a draw recorded in a
     * different subpass is refused rather than executed with the state of the
     * wrong one. Meaningless for a compute pipeline. */
    uint32_t subpass;
    /* The multisample state this pipeline was created with (DXVK262-T06): the
     * count its subpass attachment has and, when per-sample shading is on, the
     * fraction that decides how many pixel iterations the loader runs. The
     * native draw state programs the raster words from these; a hand-built
     * pipeline leaves samples at zero, which every consumer reads as one. */
    VkSampleCountFlagBits samples;
    VkBool32 sample_shading_enable;
    float min_sample_shading;
    void *graphics_state;
    void (*graphics_release)(VkDevice, void *);
    /* Viewport/scissor arrays, viewport_count of each (1..PS5VK_MAX_VIEWPORTS;
     * Vulkan requires the two counts to match). The single-element names alias
     * index zero, the viewport every draw without a ViewportIndex output uses.
     * Dynamic arrays leave these unused and take the command buffer's. */
    uint32_t viewport_count;
    union {
        VkViewport viewport; /* viewports[0] */
        VkViewport viewports[PS5VK_MAX_VIEWPORTS];
    };
    union {
        VkRect2D scissor; /* scissors[0] */
        VkRect2D scissors[PS5VK_MAX_VIEWPORTS];
    };
    VkBool32 dynamic_viewport, dynamic_scissor;
    /* Static rasterization state, and whether VK_DYNAMIC_STATE_DEPTH_BIAS was
     * declared: when it was, the three factors here are unused and a draw
     * takes them from the command buffer instead. */
    struct ps5vk_raster_state raster;
    VkBool32 dynamic_depth_bias;
    /* VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK / _WRITE_MASK / _REFERENCE. */
    VkBool32 dynamic_stencil_compare_mask, dynamic_stencil_write_mask,
        dynamic_stencil_reference;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    /* Input-assembly state, not a shader capability: the fixed-function front
     * end cuts a strip where an index matches the reset index. Accepted for the
     * strip topologies this profile carries and refused everywhere else. */
    VkBool32 primitive_restart;
    VkBool32 depth_test, depth_write;
    VkCompareOp depth_compare;
    /* Per-attachment colour state, the subpass's count in
     * color_attachment_count; consumers read element 0 while the profile
     * serves one colour attachment. */
    uint32_t color_attachment_count;
    VkFormat color_format[PS5VK_MAX_COLOR_ATTACHMENTS], depth_format;
    VkColorComponentFlags color_write_mask[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkPipelineColorBlendAttachmentState color_blend[PS5VK_MAX_COLOR_ATTACHMENTS];
    float blend_constants[4];
    uint32_t vertex_binding_count, vertex_attribute_count;
    union {
        VkVertexInputBindingDescription vertex_binding; /* first description */
        VkVertexInputBindingDescription vertex_bindings[16];
    };
    VkVertexInputAttributeDescription vertex_attributes[32];
    uint32_t code[];
};
static inline int ps5vk_graphics_set_required(VkPipeline p,unsigned set)
{
    return p && set<p->set_count && set<PS5VK_MAX_SETS && p->sets[set].count &&
        (!p->graphics_usage_known || (p->graphics_used_set_mask & (1u<<set)));
}
#endif

#ifndef PS5VK_GRAPHICS_PROGRAM_H
#define PS5VK_GRAPHICS_PROGRAM_H
#include <vulkan/vulkan_core.h>
#include "color_attachment_contract.h"
#include <stddef.h>
#include "vk_descriptor.h"
#include "graphics_stages.h"
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
    /* Optional geometry stage. A zero word_count means the pipeline has no
     * geometry stage at all, which is how every pre-T04 key is read; when it is
     * present the pre-raster stage is the merged vertex+geometry program the
     * compiler emits from this pair, not the vertex module alone. */
    struct ps5vk_graphics_module_key geometry;
    /* Optional tessellation pair. A zero word_count in tess_control means the
     * pipeline has no tessellation stages, which is how every earlier key is
     * read; the control and evaluation stages always appear together (Vulkan
     * requires it), so either module without the other is not a valid key and
     * callers use ps5vk_graphics_tessellation_key_valid() before resolving. The
     * patch control points are part of the identity rather than fixed state:
     * the control stage's output vertex count, the evaluation stage's
     * per-vertex input arrays and the tessellator's patch size are all derived
     * from them, so two pipelines that differ only there are different
     * programs and must never share a cache entry. */
    struct ps5vk_graphics_module_key tess_control, tess_eval;
    uint32_t patch_control_points;
    /* The graphics feature bits the logical device enabled, so the compiler
     * adapter can refuse a shader that consumes a capability the application
     * never enabled instead of silently delivering it. This is an acceptance
     * input rather than part of the program identity: the compiled artifact is
     * the same either way, so library matching and the compile cache ignore
     * it and the adapter checks it on every acquisition. */
    uint32_t feature_mask;
    VkPrimitiveTopology topology;
    /* Per-attachment colour state, attachment i in element i, bounded by the
     * colour-attachment contract. The count is the subpass's own. Recording
     * this contract does not imply native blend support or a second target. */
    uint32_t color_attachment_count;
    VkFormat color_format[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkSampleCountFlagBits samples;
    VkColorComponentFlags color_write_mask[PS5VK_MAX_COLOR_ATTACHMENTS];
    /* Fixed attachment state; ignored/canonicalized to zero when disabled. */
    VkBool32 blend_enable[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkBlendFactor src_color_blend_factor[PS5VK_MAX_COLOR_ATTACHMENTS], dst_color_blend_factor[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkBlendOp color_blend_op[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkBlendFactor src_alpha_blend_factor[PS5VK_MAX_COLOR_ATTACHMENTS], dst_alpha_blend_factor[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkBlendOp alpha_blend_op[PS5VK_MAX_COLOR_ATTACHMENTS];
    float blend_constants[4];
    uint32_t vertex_binding_count, vertex_attribute_count, descriptor_set_count;
    const VkVertexInputBindingDescription *vertex_bindings;
    const VkVertexInputAttributeDescription *vertex_attributes;
    const struct ps5vk_set_signature *descriptor_sets;
    uint32_t push_constant_size;
    VkShaderStageFlags push_constant_stages[PS5VK_MAX_PUSH_CONSTANT_DWORDS];
};
/* True when the pipeline carries a geometry stage. */
static inline int ps5vk_graphics_has_geometry(const struct ps5vk_graphics_key *key)
{ return key && key->geometry.words && key->geometry.word_count; }
/* True when the pipeline carries a tessellation control/evaluation pair. */
static inline int ps5vk_graphics_has_tessellation(const struct ps5vk_graphics_key *key)
{ return key && key->tess_control.words && key->tess_control.word_count; }
/* Both halves present with a patch control point count the profile bounds:
 * the shape every consumer of the tessellation key fields relies on. */
static inline int ps5vk_graphics_tessellation_key_valid(const struct ps5vk_graphics_key *key)
{
    if(!ps5vk_graphics_has_tessellation(key))return 0;
    if(!key->tess_eval.words || !key->tess_eval.word_count)return 0;
    return key->patch_control_points>0 &&
        key->patch_control_points<=PS5VK_MAX_PATCH_CONTROL_POINTS;
}
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
 * (third_party/psbc-reference libpsbc/psbc_compile.c ps5_last_provoking_vertex,
 * which already maps point list 1, line list 2 and line strip 3 to their own
 * provoking vertices) and the pinned register header (amdgfxregs.h
 * V_030908_DI_PT_POINTLIST = 1, V_030908_DI_PT_LINELIST = 2,
 * V_030908_DI_PT_LINESTRIP = 3, V_030908_DI_PT_TRILIST = 4,
 * V_030908_DI_PT_TRISTRIP = 6) agree on every value, so the shader compile and
 * the linked pipeline cannot disagree about the primitive they were built for.
 * The point and line families are resolved because they are what a device
 * advertising geometryShader is expected to feed a geometry stage with, but a
 * topology this profile has not measured as a rasterized output on its own does
 * not become accepted just because it resolves: see
 * ps5vk_agc_primitive_needs_geometry below. Triangle fan, adjacency and every
 * other topology stay fail-closed at the resolver. */
#define PS5VK_AGC_PRIMITIVE_TYPE_POINT_LIST 1u
#define PS5VK_AGC_PRIMITIVE_TYPE_LINE_LIST 2u
#define PS5VK_AGC_PRIMITIVE_TYPE_LINE_STRIP 3u
#define PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST 4u
#define PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP 6u
/* The patch-list draw's DI type: the pinned gfx103 register data names
 * DI_PT_PATCH 9 (src/amd/registers/gfx103.json). The tessellator, not the
 * assembler, generates the rasterized primitive - VGT_TF_PARAM carries that
 * shape - so the patch type only feeds the DI and the hull state. */
#define PS5VK_AGC_PRIMITIVE_TYPE_PATCH 9u
static inline int ps5vk_agc_primitive_type(VkPrimitiveTopology topology,uint32_t *out)
{
    if(!out)return -1;
    switch(topology) {
    /* Points and lines are resolved for a geometry pipeline's INPUT; a plain
     * pipeline stands or falls on ps5vk_agc_primitive_needs_geometry. */
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        *out=PS5VK_AGC_PRIMITIVE_TYPE_POINT_LIST;return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
        *out=PS5VK_AGC_PRIMITIVE_TYPE_LINE_LIST;return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
        *out=PS5VK_AGC_PRIMITIVE_TYPE_LINE_STRIP;return 0;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
        *out=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST;return 0;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        *out=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP;return 0;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        *out=PS5VK_AGC_PRIMITIVE_TYPE_PATCH;return 0;
    default:
        return -1;
    }
}
/* The tessellation compile options keep the compiler's default primitive
 * state (the compiler refuses the DI patch value as an option), so the
 * pipeline's patch type is resolved separately from the compile's. */
static inline int ps5vk_tess_patch_primitive_type(uint32_t *out)
{
    if(!out)return 0;
    *out=PS5VK_AGC_PRIMITIVE_TYPE_PATCH;
    return 1;
}
/* True for the primitive families this profile accepts only as the input of a
 * geometry stage. The geometry witness measures exactly that shape - the
 * merged program's link value, with the stage reading one vertex per point and
 * two per line - while a plain point or line pipeline has no witness behind it,
 * so it stays refused instead of riding on the geometry families' acceptance. */
static inline int ps5vk_agc_primitive_needs_geometry(uint32_t primitive_type)
{
    return primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_POINT_LIST ||
        primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_LINE_LIST ||
        primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_LINE_STRIP;
}
/* The primitive values the native create path will hand to the AGC linker. It is
 * the same set ps5vk_agc_primitive_type resolves, named separately because the
 * native path receives the resolved value rather than the topology. */
static inline int ps5vk_agc_primitive_linkable(uint32_t primitive_type)
{
    return primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_POINT_LIST ||
        primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_LINE_LIST ||
        primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_LINE_STRIP ||
        primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST ||
        primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP;
}
VkResult ps5vk_graphics_resolve(const struct ps5vk_graphics_library *,
    const struct ps5vk_graphics_key *, const struct ps5vk_graphics_program **);
#endif

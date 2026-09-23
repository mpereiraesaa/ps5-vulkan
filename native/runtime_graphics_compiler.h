#ifndef PS5VK_RUNTIME_GRAPHICS_COMPILER_H
#define PS5VK_RUNTIME_GRAPHICS_COMPILER_H
#include "graphics_program.h"
#include "runtime_shader.h"

struct ps5vk_runtime_graphics_program {
    /* The pre-raster program: a vertex-only compile, the merged vertex+geometry
     * program, or zero for a tessellation pipeline, whose pre-raster state is
     * the hull and domain programs below. The loader gate keeps refusing the
     * tessellation halves while their pipeline state is unwritten. */
    PsbcShaderOutput vertex,fragment;
    /* Tessellation pair. The hull is the ONE merged LS/HS image
     * psbc_compile_tess_pipeline emits by compiling the vertex and control
     * halves together, and the domain is the TES NGG package the evaluation
     * half compiles to. Both are zero for every earlier pipeline shape. */
    PsbcShaderOutput hull,domain;
    /* DIAGNOSTIC (PS5VK_TESS_LEGACY_DOMAIN, default off): the evaluation half
     * compiled a SECOND time as a legacy hardware vertex shader - VS_STAGE_DS,
     * no NGG - with its own draw ABI, so the patch draw can launch that shape
     * instead of the NGG one. The NGG domain above stays: it is what the
     * platform's shader constructor and linker accept, so the linked pixel
     * interpolation still comes from it. Zero unless the knob is on. */
    PsbcShaderOutput domain_legacy;
    struct ps5vk_runtime_draw_abi arguments_legacy;
    uint32_t domain_legacy_valid;
    /* Independent input assembly and validated TCS OutputVertices counts.
     * Both zero without the tessellation pair. */
    uint32_t patch_control_points;
    uint32_t tess_output_points;
    struct ps5vk_runtime_draw_abi arguments;
    struct ps5vk_runtime_draw_abi hull_arguments;
    /* Derived from the exact fragment metadata pair 0x44/0xff. It is compiler
     * evidence only; feature enablement and SRC1 use are separate gates. */
    uint32_t dual_source_export;
    /* The shape that pair describes, decided by the fragment interface because
     * the registers alone cannot tell dual source from two MRTs:
     * PS5VK_RUNTIME_FRAGMENT_SHAPE_SINGLE, _DUAL or _TWO_MRT. */
    uint32_t fragment_shape;
    /* The GFX1013 primitive this pair was compiled for, resolved from the key's
     * topology. The native create path links the pair with a primitive the
     * caller supplies, and refuses any value that is not this one, so a
     * compiled shader and its linked pipeline cannot end up describing
     * different primitives. */
    uint32_t primitive_type;
};
/* Uncached compiler adapter. The acquisition seam may wrap this with caching.
 * The consumer must copy code/metadata before the lease is released. */
VkResult ps5vk_runtime_graphics_compile(void *,const struct ps5vk_graphics_key *,const void **);
void ps5vk_runtime_graphics_free(void *,const void *);
int ps5vk_runtime_graphics_supported(const struct ps5vk_graphics_key *);
/* Diagnostic-only: the condition the adapter refused a key on, and the PSBC
 * result of the last failed compile. The SDk diagnostic build logs the key
 * fields when PS5VK_GEOMETRY_KEY_DIAG is defined; nothing read here decides
 * anything in a shipping build. */
extern unsigned ps5vk_runtime_graphics_diag_site;
extern int ps5vk_runtime_graphics_diag_result;
/* Shared descriptor-table lowering, independent of the currently enabled
 * draw ABI. Success proves compiler options only, not native submission. */
VkResult ps5vk_runtime_graphics_descriptor_options(const struct ps5vk_graphics_key *,
    VkShaderStageFlags, PsbcCompileOptions *);
VkResult ps5vk_runtime_graphics_t08_options(uint32_t feature_mask,
    PsbcCompileOptions *options);
/* True when the compiled pair only consumes capabilities the logical device
 * enabled. The compiled metadata is the usage evidence: a distance array a
 * shader declares but never writes does not require its feature, while one the
 * compiler really exports does. */
int ps5vk_runtime_graphics_feature_use_ok(const PsbcShaderMetadata *pre_raster,
    const PsbcShaderMetadata *fragment,uint32_t feature_mask);
/* True when both halves describe the fragment stage's clip/cull distance reads
 * end to end: the pre-raster stage names each packed distance register it
 * exports (with the parameter index above the private key), the pixel stage
 * names the same registers as inputs, and the exports cover the declared reads.
 * The shipping profile still refuses to RUN one until a native witness exists;
 * this is the description half, which is what a host test can check against the
 * real compiled metadata. */
int ps5vk_runtime_graphics_distance_reads_described(const PsbcShaderMetadata *pre_raster,
    const PsbcShaderMetadata *fragment,unsigned declared_clip,unsigned declared_cull);
/* Context is an existing ps5vk_compilation_cache. Lease data has the same
 * program view as the uncached adapter, but must use cached_release. */
VkResult ps5vk_runtime_graphics_cached_acquire(void *,const struct ps5vk_graphics_key *,const void **);
void ps5vk_runtime_graphics_cached_release(void *,const void *);
#endif

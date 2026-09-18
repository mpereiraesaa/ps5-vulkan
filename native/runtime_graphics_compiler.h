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
    /* Tessellation pair. The hull is the LS+HS program psbc_compile_tess_pipeline
     * links (HS machine code at offset 0, the vertex half behind it, hull_ls_*
     * publication), the domain the TES NGG package the evaluation half compiles
     * to. Both are zero for every earlier pipeline shape. */
    PsbcShaderOutput hull,domain;
    /* The pipeline's input patch control points, the tessellation launch
     * state's input/output control-point counts. Zero without the pair. */
    uint32_t patch_control_points;
    struct ps5vk_runtime_draw_abi arguments;
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

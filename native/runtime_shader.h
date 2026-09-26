#ifndef PS5VK_RUNTIME_SHADER_H
#define PS5VK_RUNTIME_SHADER_H

#include "ps5_shader_header.h"
#include "libpsbc/psbc_compile.h"
#include "runtime_draw_abi.h"
#include "runtime_shader_storage.h"
#include "runtime_fragment_shape.h"

/* Separate arena for compiler output; the frozen Gears arena stays unchanged.
 * All pointers in an uncreated header are field-relative, as required by AGC.
 * The owner must retain this storage and the code through GPU retirement. */
_Static_assert(PS5VK_RUNTIME_CX_MAX >= PSBC_MAX_CONTEXT_REGISTERS,"compiler CX capacity");
_Static_assert(PS5VK_RUNTIME_SH_MAX >= PSBC_MAX_SHADER_REGISTERS,"compiler SH capacity");
_Static_assert(PS5VK_RUNTIME_SEMANTICS_MAX >= PSBC_MAX_SEMANTICS,"compiler semantics capacity");

enum ps5vk_runtime_fragment_export {
    PS5VK_RUNTIME_FRAGMENT_EXPORT_NONE=0,
    PS5VK_RUNTIME_FRAGMENT_EXPORT_SINGLE=1,
    PS5VK_RUNTIME_FRAGMENT_EXPORT_DUAL=2,
    /* One export, and the shader mask says it targets the SECOND colour
     * target: the fragment stage exports Location 1 only, which is what the
     * pinned render-pass module's attachment_write_mask leaf builds when the
     * first target's write mask is zero. The compiler publishes that shape as
     * SPI_SHADER_COL_FORMAT=0x9 with CB_SHADER_MASK=0xf0: the format nibbles
     * follow the module's outputs in DECLARATION order while the mask follows
     * the attachment each export targets (measured on the pinned PSBC). */
    PS5VK_RUNTIME_FRAGMENT_EXPORT_SINGLE_SECOND=3
};
/* Classify the exact compiler-produced fragment export register pair. Returns
 * -1 for a torn/unknown pair. This is metadata validation, not device feature
 * authorization: a DUAL result still cannot reach SRC1 blend state until the
 * pipeline carries the enabled dualSrcBlend contract. */
int ps5vk_runtime_fragment_export(const PsbcShaderMetadata *metadata);

/* VK_EXT_transform_feedback (DXVK262-T14): whether a compiled pre-raster
 * program captures into exactly the XfbBuffer mask `buffers` (zero: captures
 * nothing). A capture is described only for the merged vertex+geometry
 * program: its streamout table pointer in a user-data dword, a stride for
 * every written buffer, and each buffer on one stream. */
int ps5vk_runtime_streamout_matches(const PsbcShaderMetadata *metadata, uint32_t buffers);

/* Header construction only. Code upload, cache publication, AGC creation and
 * linking remain caller responsibilities. The bounded profile permits the
 * compiler-described vertex-buffer table and one fragment combined-image
 * sampler at set 0/binding 0. Other descriptor profiles fail before writes. */
int ps5vk_runtime_shader_build(struct ps5vk_runtime_shader *destination,
                              const PsbcShaderOutput *compiled);
/* The merged LS/HS program's loader view, built without the AGC linker. It is
 * ONE program: GFX9+ runs the vertex and control halves as a single hardware
 * stage, and the compiler emits a single image whose halves dispatch on
 * merged_wave_info. The pgm lo/hi values stay zero; the native create path
 * patches the real address after loading the code. The tessellation launch
 * state the hull still needs (stage enables, LS_HS_CONFIG, the rings, and the
 * LDS allocation) is the create path's, derived from the hull metadata's
 * workgroup layout. */
int ps5vk_runtime_hull_build(struct ps5vk_runtime_shader *hull,
    const PsbcShaderOutput *compiled);
/* Independent LS/HS user-data block. No NGG LDS-layout slot or pixel bank. */
int ps5vk_runtime_hull_abi_build(const PsbcShaderMetadata *hull,
    struct ps5vk_runtime_draw_abi *out);
int ps5vk_runtime_draw_abi_build(const PsbcShaderMetadata *vertex,
    const PsbcShaderMetadata *fragment,struct ps5vk_runtime_draw_abi *out);

#endif

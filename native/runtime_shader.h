#ifndef PS5VK_RUNTIME_SHADER_H
#define PS5VK_RUNTIME_SHADER_H

#include "ps5_shader_header.h"
#include "libpsbc/psbc_compile.h"
#include "runtime_draw_abi.h"
#include "runtime_shader_storage.h"

/* Separate arena for compiler output; the frozen Gears arena stays unchanged.
 * All pointers in an uncreated header are field-relative, as required by AGC.
 * The owner must retain this storage and the code through GPU retirement. */
_Static_assert(PS5VK_RUNTIME_CX_MAX >= PSBC_MAX_CONTEXT_REGISTERS,"compiler CX capacity");
_Static_assert(PS5VK_RUNTIME_SH_MAX >= PSBC_MAX_SHADER_REGISTERS,"compiler SH capacity");
_Static_assert(PS5VK_RUNTIME_SEMANTICS_MAX >= PSBC_MAX_SEMANTICS,"compiler semantics capacity");

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
int ps5vk_runtime_draw_abi_build(const PsbcShaderMetadata *vertex,
    const PsbcShaderMetadata *fragment,struct ps5vk_runtime_draw_abi *out);

#endif

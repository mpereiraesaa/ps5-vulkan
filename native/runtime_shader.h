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
 * linking remain caller responsibilities. Descriptor-free NGG VS / pixel FS
 * profile; unsupported resource requirements fail before destination writes. */
int ps5vk_runtime_shader_build(struct ps5vk_runtime_shader *destination,
                              const PsbcShaderOutput *compiled);
int ps5vk_runtime_draw_abi_build(const PsbcShaderMetadata *vertex,
    const PsbcShaderMetadata *fragment,struct ps5vk_runtime_draw_abi *out);

#endif

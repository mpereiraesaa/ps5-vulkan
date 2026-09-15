/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shader draw parameters (VK_KHR_shader_draw_parameters, Vulkan 1.1).
 *
 * The pinned frontend maps the Vulkan built-in BaseVertex to
 * SYSTEM_VALUE_FIRST_VERTEX and says so explicitly
 * (third_party/psbc-reference/src/compiler/spirv/vtn_variables.c:1062-1067:
 * "OpenGL gl_BaseVertex (SYSTEM_VALUE_BASE_VERTEX) is not the same semantic as
 * Vulkan BaseVertex (SYSTEM_VALUE_FIRST_VERTEX)"), and
 * ac_nir_lower_intrinsics_to_args.c:315 lowers that to args->base_vertex, the
 * user-data pair PSBC exports to us. The value the shader must therefore
 * observe is:
 *
 *   non-indexed draw: firstVertex
 *   indexed draw:     the signed vertexOffset (two's complement, negatives
 *                     preserved)
 *
 * That mapping is what the pinned upstream CTS requires, not a convenience:
 * vktDrawShaderDrawParametersTests.cpp:422 draws with firstVertex = 2 and the
 * pinned shader VertexFetchShaderDrawParameters.vert accepts only when
 * (gl_VertexIndex - gl_BaseVertexARB) == in_refVertexIndex, i.e. BaseVertex = 2;
 * the indexed cases pass vertexOffset = 1 and would fail if the driver fed
 * firstVertex (which Vulkan requires to be zero for indexed draws). RADV fills
 * the same SGPR the same way (radv_cmd_buffer.c:11703-11705 for non-indexed,
 * :11666 for indexed). BaseInstance is the command's firstInstance.
 *
 * DrawIndex is NOT implemented and is not claimed here. The pinned compiler
 * does lower SpvBuiltInDrawIndex to its draw_id argument
 * (vtn_variables.c:1076, ac_nir_lower_intrinsics_to_args.c:321) and RADV places
 * that in the same user-data block at base+8 (radv_shader_args.c:209-210),
 * but psbc_compile.h exports no user-data slot for it and ps5vk has no
 * multi-draw path, so a shader using gl_DrawIndex would read an SGPR the
 * runtime never writes. ps5vk_draw_index_supported() therefore stays false and
 * the public feature bit stays unadvertised until both exist.
 */
#ifndef PS5VK_DRAW_PARAMETERS_H
#define PS5VK_DRAW_PARAMETERS_H
#include <stdint.h>
#include "vk_command.h"

static inline uint32_t ps5vk_draw_base_vertex(const struct ps5vk_operation *op)
{
    if (!op) return 0;
    return op->type == PS5VK_DRAW_INDEXED ? (uint32_t)op->vertex_offset
                                          : op->first_vertex;
}

static inline uint32_t ps5vk_draw_base_instance(const struct ps5vk_operation *op)
{
    return op ? op->first_instance : 0;
}

/* False until the compiler exports a draw_id user-data slot and a multi-draw
 * path exists to give it a real sequence number. */
static inline uint32_t ps5vk_draw_index_supported(void)
{
    return 0u;
}

#endif

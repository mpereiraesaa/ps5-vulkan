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
 * DrawIndex IS delivered for the draws this profile supports. The pinned
 * compiler lowers SpvBuiltInDrawIndex to its draw_id argument and reports the
 * user-data slot for it (psbc_compile.h, metadata version 13), the runtime ABI
 * mirrors that slot, and ps5vk_draw_index_value() supplies the value the shader
 * must observe: the pinned specification defines DrawIndex as zero for every
 * direct draw and as the index of the command within a vkCmdDraw*Indirect
 * call, beginning at zero for each call. The recorded operation carries zero;
 * vk_indirect.c assigns the command index to each resolved snapshot at the
 * queue head (ps5vk_indirect_resolve_command), including commands that draw
 * no primitive, so the sequence is never compacted or renumbered and never
 * accumulates across calls.
 *
 * The direct/single-indirect contract is independently witnessed and
 * advertised through the device platform mask. The legacy diagnostic
 * supported() helper below is not consulted by public feature queries.
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

/* The DrawIndex a shader must observe for one draw: the command index the
 * queue-head resolution stored in the snapshot, zero for a recorded direct
 * draw and for the first command of any indirect call. */
static inline uint32_t ps5vk_draw_index_value(const struct ps5vk_operation *op)
{
    return op ? op->draw_index : 0u;
}

/* Default ViewIndex for the single-view path. Multiview replay supplies the
 * actual view explicitly to both stages' independently assigned ABI slots;
 * this fallback is not the public capability query. */
static inline uint32_t ps5vk_draw_view_index_value(const struct ps5vk_operation *op)
{
    (void)op;
    return 0u;
}

/* Legacy single-draw diagnostic helper, not a public advertisement gate.
 * Device queries use the platform feature mask. Multi-draw stays unsupported. */
static inline uint32_t ps5vk_draw_index_supported(void)
{
    return 0u;
}

/* Legacy single-view diagnostic helper, not the public multiview feature.
 * Real multiview support is queried through the platform/device feature mask. */
static inline uint32_t ps5vk_draw_view_index_supported(void)
{
    return 0u;
}

#endif

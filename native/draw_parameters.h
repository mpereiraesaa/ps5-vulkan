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
 * direct draw and as beginning at zero for indirect draws, and ps5vk refuses
 * more than one draw per command (multiDrawIndirect is false), so every draw
 * it executes is that first draw and the value is zero.
 *
 * What is still missing is the evidence and one dependent capability, not the
 * value: the pinned CTS leaves that exercise DrawIndex with non-zero values are
 * the multi-draw ones, ps5vk has no multi-draw path (T03), and no native
 * witness has been taken for the DrawIndex slot yet. The public feature bit is
 * therefore still not advertised, and ps5vk_draw_index_supported() reports
 * that advertisement gate rather than whether a value can be delivered.
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

/* The DrawIndex a shader must observe for one recorded draw. ps5vk executes at
 * most one draw per command, so this is the first draw of the command and the
 * specification makes that zero. A future multi-draw path (T03) must pass its
 * sequence index here instead of a constant. */
static inline uint32_t ps5vk_draw_index_value(const struct ps5vk_operation *op)
{
    (void)op;
    return 0u;
}

/* The public advertisement gate: false until the DrawIndex evidence exists.
 * Delivery is not the blocker - the multi-draw CTS leaves that judge non-zero
 * DrawIndex values are, plus the missing native witness. */
static inline uint32_t ps5vk_draw_index_supported(void)
{
    return 0u;
}

#endif

/* Shader draw-parameter contract pinning (VK_KHR_shader_draw_parameters).
 *
 * This file is deliberately a contract and negative-regression test, not an
 * enablement: the public feature stays false because DrawIndex is still
 * unimplemented and the runtime path still refuses indexed draws. What is
 * pinned here is the exact value mapping the pinned upstream CTS requires and
 * the exact gaps that keep the feature unadvertised, so neither can drift
 * silently while the tranche is in progress.
 */
#include "draw_parameters.h"
#include "runtime_draw_abi.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    /* BaseVertex for a non-indexed draw is firstVertex: the pinned CTS draws
     * with firstVertex = 2 and its shader accepts only when
     * gl_VertexIndex - gl_BaseVertexARB == 2. */
    struct ps5vk_operation draw = {
        .type = PS5VK_DRAW, .vertex_count = 4, .instance_count = 3,
        .first_vertex = 2, .first_instance = 5};
    assert(ps5vk_draw_base_vertex(&draw) == 2u);
    assert(ps5vk_draw_base_instance(&draw) == 5u);

    /* BaseVertex for an indexed draw is the signed vertexOffset, never the
     * first vertex, and a negative offset survives as two's complement. */
    struct ps5vk_operation indexed = {
        .type = PS5VK_DRAW_INDEXED, .index_count = 4, .instance_count = 1,
        .first_vertex = 0, .vertex_offset = 1, .first_instance = 0};
    assert(ps5vk_draw_base_vertex(&indexed) == 1u);
    indexed.vertex_offset = -2;
    assert(ps5vk_draw_base_vertex(&indexed) == UINT32_MAX - 1u);
    indexed.vertex_offset = 2147483647;
    assert(ps5vk_draw_base_vertex(&indexed) == 2147483647u);
    indexed.first_instance = 7;
    assert(ps5vk_draw_base_instance(&indexed) == 7u);

    /* A non-indexed draw keeps using firstVertex even when it is non-zero,
     * because the frontend maps the Vulkan built-in to
     * SYSTEM_VALUE_FIRST_VERTEX; a caller that fed zero here would fail the
     * pinned base_vertex CTS cases by construction. */
    draw.first_vertex = 11;
    assert(ps5vk_draw_base_vertex(&draw) == 11u);

    /* The runtime data flow must publish exactly those values into the
     * compiler-declared slots, and nothing else. */
    struct ps5vk_runtime_draw_abi abi = {
        .enabled = 1, .vertex_count = 3, .fragment_count = 1,
        .base_vertex_slot = 0, .start_instance_slot = 1, .lds_slot = 2, .lds_value = 0,
        .vertex_push_slot = UINT32_MAX, .fragment_push_slot = UINT32_MAX};
    uint32_t vertex[16], pixel[16];
    memset(vertex, 0, sizeof(vertex));
    memset(pixel, 0, sizeof(pixel));
    assert(ps5vk_runtime_draw_values_sets(&abi, ps5vk_draw_base_vertex(&draw),
        ps5vk_draw_base_instance(&draw), 0, 0, NULL, vertex, pixel) == -1);
    assert(ps5vk_runtime_draw_values_sets(&abi, ps5vk_draw_base_vertex(&draw),
        ps5vk_draw_base_instance(&draw), 0, 0, (const uint32_t[4]){0, 0, 0, 0},
        vertex, pixel) == 0);
    assert(vertex[abi.base_vertex_slot] == 11u);
    assert(vertex[abi.start_instance_slot] == 5u);

    memset(vertex, 0, sizeof(vertex));
    assert(ps5vk_runtime_draw_values_sets(&abi, ps5vk_draw_base_vertex(&indexed),
        ps5vk_draw_base_instance(&indexed), 0, 0, (const uint32_t[4]){0, 0, 0, 0},
        vertex, pixel) == 0);
    assert(vertex[abi.base_vertex_slot] == 2147483647u);
    assert(vertex[abi.start_instance_slot] == 7u);

    /* The DrawIndex gap is pinned rather than papered over: no compiler slot is
     * exported for it, the helper reports it unsupported, and the feature bit
     * the public query publishes stays false until both change. */
    assert(ps5vk_draw_index_supported() == 0u);
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Static_assert(offsetof(struct ps5vk_runtime_draw_abi, base_vertex_slot) !=
                   offsetof(struct ps5vk_runtime_draw_abi, start_instance_slot),
                   "base vertex and start instance must stay distinct slots");
#endif
    return 0;
}

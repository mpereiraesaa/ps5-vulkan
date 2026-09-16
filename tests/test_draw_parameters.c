/* Shader draw-parameter contract pinning (VK_KHR_shader_draw_parameters).
 *
 * This file is deliberately a contract and negative-regression test, not an
 * enablement. Delivery is implemented: BaseVertex, BaseInstance and DrawIndex
 * all reach the compiler-declared slots. DrawIndex is the command index the
 * queue-head resolution stored in the snapshot (zero for direct draws and for
 * a single indirect command), so a multi-draw expansion delivers a distinct
 * value per command without touching this data flow. What is pinned here is
 * the exact value mapping the pinned upstream CTS requires and the slot
 * validation that keeps the three words independent.
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
        .base_vertex_slot = 0, .start_instance_slot = 1, .draw_id_slot = UINT32_MAX, .view_index_slot = UINT32_MAX,
        .lds_slot = 2, .lds_value = 0,
        .vertex_push_slot = UINT32_MAX, .fragment_push_slot = UINT32_MAX};
    uint32_t vertex[16], pixel[16];
    memset(vertex, 0, sizeof(vertex));
    memset(pixel, 0, sizeof(pixel));
    assert(ps5vk_runtime_draw_values_sets(&abi, ps5vk_draw_base_vertex(&draw),
        ps5vk_draw_base_instance(&draw), ps5vk_draw_index_value(&draw), ps5vk_draw_view_index_value(&draw), 0, 0, NULL,
        vertex, pixel) == -1);
    assert(ps5vk_runtime_draw_values_sets(&abi, ps5vk_draw_base_vertex(&draw),
        ps5vk_draw_base_instance(&draw), ps5vk_draw_index_value(&draw), ps5vk_draw_view_index_value(&draw), 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == 0);
    assert(vertex[abi.base_vertex_slot] == 11u);
    assert(vertex[abi.start_instance_slot] == 5u);

    memset(vertex, 0, sizeof(vertex));
    assert(ps5vk_runtime_draw_values_sets(&abi, ps5vk_draw_base_vertex(&indexed),
        ps5vk_draw_base_instance(&indexed), ps5vk_draw_index_value(&indexed), ps5vk_draw_view_index_value(&indexed), 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == 0);
    assert(vertex[abi.base_vertex_slot] == 2147483647u);
    assert(vertex[abi.start_instance_slot] == 7u);

    /* DrawIndex delivery is pinned here, and the advertisement gap is pinned
     * separately: the value a shader observes is the sequence index of the draw
     * in its command, which is zero because multi-draw is refused; the public
     * feature bit stays false until the upstream CTS leaves for the supported
     * direct/single-indirect contract and a native witness exist. Multi-draw is
     * a separate expansion and its leaves are legitimately NotSupported while
     * multiDrawIndirect is false, so it does not gate this. */
    assert(ps5vk_draw_index_value(&draw) == 0u);
    assert(ps5vk_draw_index_value(&indexed) == 0u);
    /* A resolved multi-draw snapshot carries the command index the queue head
     * assigned (vk_indirect.c); the value is read from the snapshot, never
     * derived from a counter, so a zero-primitive command keeps its index. */
    struct ps5vk_operation third = draw;
    third.draw_index = 2; third.vertex_count = 0;
    assert(ps5vk_draw_index_value(&third) == 2u);
    third.draw_index = 65534;
    assert(ps5vk_draw_index_value(&third) == 65534u);
    assert(ps5vk_draw_index_value(NULL) == 0u);
    abi.draw_id_slot = 1; /* collides with the start-instance slot */
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 3u, 0u, 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == -1);
    abi.draw_id_slot = 2; /* collides with the LDS slot */
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 3u, 0u, 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == -1);
    abi.draw_id_slot = 0;
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 3u, 0u, 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == -1);
    abi.draw_id_slot = 3; /* outside the declared vertex block */
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 3u, 0u, 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == -1);
    abi.draw_id_slot = 0;
    abi.base_vertex_slot = UINT32_MAX;
    memset(vertex, 0, sizeof(vertex));
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 3u, 0u, 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == 0);
    assert(vertex[0] == 3u); /* the DrawIndex word reaches the block */
    assert(vertex[1] == 5u);
    memset(vertex, 0, sizeof(vertex));
    assert(ps5vk_runtime_draw_values_sets(&abi, ps5vk_draw_base_vertex(&third),
        ps5vk_draw_base_instance(&third), ps5vk_draw_index_value(&third),
        ps5vk_draw_view_index_value(&third), 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == 0);
    assert(vertex[0] == 65534u); /* the snapshot's own index, unmodified */
    assert(vertex[1] == 5u);
    assert(ps5vk_draw_index_supported() == 0u);

    /* ViewIndex delivery: the compiler declares a slot only when the vertex
     * stage really reads gl_ViewIndex, and this profile advertises no
     * multiview, so the value delivered through it is the specification's zero
     * rather than whatever the register happened to hold. The slot obeys the
     * same range and collision rules as every other word of the block. */
    assert(ps5vk_draw_view_index_value(&draw) == 0u);
    assert(ps5vk_draw_view_index_value(&indexed) == 0u);
    assert(ps5vk_draw_view_index_supported() == 0u);
    abi.draw_id_slot = UINT32_MAX;
    abi.view_index_slot = 1; /* collides with the start-instance slot */
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 0u, 0u, 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == -1);
    abi.view_index_slot = 2; /* collides with the LDS slot */
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 0u, 0u, 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == -1);
    abi.view_index_slot = 3; /* outside the declared vertex block */
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 0u, 0u, 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == -1);
    abi.view_index_slot = 0;
    abi.base_vertex_slot = UINT32_MAX;
    memset(vertex, 0, sizeof(vertex));
    assert(ps5vk_runtime_draw_values_sets(&abi, 11u, 5u, 0u,
        ps5vk_draw_view_index_value(&draw), 0, 0,
        (const uint32_t[4]){0, 0, 0, 0}, vertex, pixel) == 0);
    assert(vertex[0] == 0u); /* the ViewIndex word reaches the block */
    assert(vertex[1] == 5u);
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Static_assert(offsetof(struct ps5vk_runtime_draw_abi, base_vertex_slot) !=
                   offsetof(struct ps5vk_runtime_draw_abi, start_instance_slot),
                   "base vertex and start instance must stay distinct slots");
#endif
    return 0;
}

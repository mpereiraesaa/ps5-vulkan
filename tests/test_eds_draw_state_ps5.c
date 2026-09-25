/* VK_EXT_extended_dynamic_state at the native encoder (DXVK262-T10).
 *
 * The recorder resolves a draw's cull mode, front face and depth-test state
 * into its raster snapshot and sets fixed_function_resolved. The encoder must
 * then program PA_SU_SC_MODE_CNTL (0x205) and DB_DEPTH_CONTROL (0x200) from
 * the snapshot and not from the pipeline's static values; a snapshot without
 * the flag (every caller older than the extension) keeps the pipeline's
 * values. Register-only fixture, the same shape as test_draw_state_ps5.c. */
#include "draw_state_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t last_cx(const struct ps5vk_draw_state *s, uint32_t offset)
{
    for (unsigned i = s->cx_count; i > 0; --i)
        if (s->cx[i - 1].offset == offset) return s->cx[i - 1].value;
    assert(!"missing context register");
    return 0;
}

int main(void)
{
    struct VkDevice_T device = {0};
    struct ps5vk_graphics_pair pair = {.ready = 1, .vertex_quantization = 0x2d};
    pair.gs.specials.draw_modifier = 5;
    pair.gs.sh[2] = (ps5_agc_register){0x8a, 0x123456};
    pair.ps.sh[4] = (ps5_agc_register){0xa, 0xabcdef};
    struct ps5vk_native_graphics_pipeline native = {.device = &device, .pair = &pair};
    struct VkPipeline_T p = {.device = &device, .graphics = 1, .graphics_state = &native,
        .viewport_count = 1, .viewport = {0, 0, 64, 64, 0, 1}, .scissor = {{0, 0}, {64, 64}},
        .color_format = {VK_FORMAT_R8G8B8A8_UNORM}, .color_attachment_count = 1,
        .cull_mode = VK_CULL_MODE_BACK_BIT, .front_face = VK_FRONT_FACE_CLOCKWISE,
        .depth_compare = VK_COMPARE_OP_LESS};
    static const uint32_t offsets[16] = {0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f, 0x321, 0x323,
        0x324, 0x325, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8};
    struct ps5vk_target_registers color = {.count = 16};
    struct ps5vk_target_registers depth = {.count = PS5_DEPTH_REGISTER_COUNT};
    for (unsigned i = 0; i < 16; ++i) color.registers[i].offset = offsets[i];
    VkRect2D area = {{0, 0}, {64, 64}};
    struct ps5vk_draw_state out;

    /* Positive control: an unresolved snapshot programs the pipeline's
     * static BACK culling and CLOCKWISE front face (bits 0..1 and 2). */
    struct ps5vk_raster_state raster = {0};
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, NULL,
        &area, 64, 64, 0, &out) == VK_SUCCESS);
    const uint32_t polygon_bits = (2u << 5) | (2u << 8);
    assert(last_cx(&out, 0x205) == (VK_CULL_MODE_BACK_BIT | (1u << 2) | polygon_bits));

    /* A resolved snapshot wins: FRONT culling, COUNTER_CLOCKWISE, exactly the
     * values DXVK sets with vkCmdSetCullMode/vkCmdSetFrontFace. */
    raster.fixed_function_resolved = VK_TRUE;
    raster.cull_mode = VK_CULL_MODE_FRONT_BIT;
    raster.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, NULL,
        &area, 64, 64, 0, &out) == VK_SUCCESS);
    assert(last_cx(&out, 0x205) == (VK_CULL_MODE_FRONT_BIT | polygon_bits));
    raster.cull_mode = VK_CULL_MODE_NONE;
    raster.front_face = VK_FRONT_FACE_CLOCKWISE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, NULL,
        &area, 64, 64, 0, &out) == VK_SUCCESS);
    assert(last_cx(&out, 0x205) == ((1u << 2) | polygon_bits));

    /* A resolved value outside the enum is refused like a static one. */
    raster.cull_mode = 4u;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, NULL,
        &area, 64, 64, 0, &out) == VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    raster.cull_mode = VK_CULL_MODE_NONE;
    raster.front_face = (VkFrontFace)2;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, NULL,
        &area, 64, 64, 0, &out) == VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    raster.front_face = VK_FRONT_FACE_CLOCKWISE;

    /* Depth test state: the pipeline has the test disabled, the snapshot
     * enables it with writes and GREATER_OR_EQUAL (DB_DEPTH_CONTROL: Z_ENABLE
     * bit 1, Z_WRITE_ENABLE bit 2, ZFUNC bits 4..6). */
    p.depth_format = VK_FORMAT_D32_SFLOAT;
    raster = (struct ps5vk_raster_state){0};
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth,
        &area, 64, 64, 0, &out) == VK_SUCCESS);
    assert(last_cx(&out, 0x200) == 0u);
    raster.fixed_function_resolved = VK_TRUE;
    raster.cull_mode = p.cull_mode;
    raster.front_face = p.front_face;
    raster.depth_test = VK_TRUE;
    raster.depth_write = VK_TRUE;
    raster.depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth,
        &area, 64, 64, 0, &out) == VK_SUCCESS);
    assert(last_cx(&out, 0x200) == (2u | 4u | ((uint32_t)VK_COMPARE_OP_GREATER_OR_EQUAL << 4)));
    /* Writes without the test stay off, as Vulkan requires. */
    raster.depth_test = VK_FALSE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth,
        &area, 64, 64, 0, &out) == VK_SUCCESS);
    assert(last_cx(&out, 0x200) == 0u);
    /* And the resolved snapshot also wins in the other direction: the pipeline
     * enables the test, the draw disabled it. */
    p.depth_test = p.depth_write = VK_TRUE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth,
        &area, 64, 64, 0, &out) == VK_SUCCESS);
    assert(last_cx(&out, 0x200) == 0u);
    raster.fixed_function_resolved = VK_FALSE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth,
        &area, 64, 64, 0, &out) == VK_SUCCESS);
    assert(last_cx(&out, 0x200) == (2u | 4u | ((uint32_t)VK_COMPARE_OP_LESS << 4)));
    puts("eds draw state ps5 tests passed");
    return 0;
}

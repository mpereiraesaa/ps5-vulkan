#include "draw_state_ps5.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    /* Register-only fixture. No native shader objects or GPU execution. */
    struct VkDevice_T device = {0};
    struct ps5vk_graphics_pair pair = {.ready = 1, .vertex_quantization=0x2d};
    pair.gs.specials.draw_modifier = 5;
    pair.gs.sh[2] = (ps5_agc_register){0x8a, 0x123456};
    pair.ps.sh[4] = (ps5_agc_register){0xa, 0xabcdef};
    struct ps5vk_native_graphics_pipeline native = {.device = &device, .pair = &pair};
    struct VkPipeline_T p = {.device = &device, .graphics = 1, .graphics_state = &native,
        .viewport = {0,0,640,480,0,1}, .scissor = {{0,0},{640,480}},
        .color_format = VK_FORMAT_B8G8R8A8_UNORM, .cull_mode = VK_CULL_MODE_BACK_BIT,
        .front_face = VK_FRONT_FACE_CLOCKWISE};
    static const uint32_t offsets[16] = {0x318,0x31b,0x31c,0x31d,0x31e,0x31f,0x321,0x323,
        0x324,0x325,0x390,0x398,0x3a0,0x3a8,0x3b0,0x3b8};
    struct ps5vk_target_registers color = {.count = 16}, depth = {.count = PS5_DEPTH_REGISTER_COUNT};
    for (unsigned i = 0; i < 16; ++i) color.registers[i].offset = offsets[i];
    VkRect2D area = {{0,0},{640,480}};
    struct ps5vk_draw_state out;
    assert(ps5vk_native_draw_state(&p, &color, NULL, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx_count == 92 && out.modifier == 5);
    assert(out.cx[91].offset==0x2f9 && out.cx[91].value==0x2d);
    assert(out.cx[90].offset==0x204 && out.cx[90].value==0x01080000);
    assert(out.cx[87].offset==0x292 && out.cx[87].value==0x22);
    assert(out.cx[88].offset==0x094 && out.cx[88].value==0x80000000u);
    assert(out.cx[89].offset==0x095 && out.cx[89].value==(640u|(480u<<16)));
    assert(out.sh[2].value == 0x123456 && out.sh[10].value == 0xabcdef);
    assert(out.cx[84].value == 0 && out.cx[85].value == 0x246);
    assert(out.cx[86].offset==0x206 && out.cx[86].value==0x43f); /* homogeneous W, not reciprocal */
    float scale; memcpy(&scale, &out.cx[18].value, sizeof(scale)); assert(scale == 240);
    p.scissor=(VkRect2D){{100,50},{128,128}};
    assert(ps5vk_native_draw_state(&p,&color,NULL,&area,640,480,&out)==VK_SUCCESS);
    assert(out.cx[28].offset==0x090 && out.cx[28].value==0x80000000u);
    assert(out.cx[29].offset==0x091 && out.cx[29].value==(640u|(480u<<16)));
    assert(out.cx[88].value==(0x80000000u|100u|(50u<<16)));
    assert(out.cx[89].value==(228u|(178u<<16)));
    p.depth_format = VK_FORMAT_D32_SFLOAT; p.depth_test = p.depth_write = VK_TRUE;
    p.depth_compare = VK_COMPARE_OP_LESS;
    assert(ps5vk_native_draw_state(&p, &color, &depth, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx_count == PS5VK_DRAW_CX_CAPACITY && out.cx[106].value == 0x16);
    assert(out.cx[112].offset==0x204 && out.cx[112].value==0x01080000);
    assert(out.cx[113].offset==0x2f9 && out.cx[113].value==0x2d);
    p.depth_test = VK_FALSE;
    assert(ps5vk_native_draw_state(&p, &color, &depth, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx[106].value == 0);
    pair.ready = 0;
    assert(ps5vk_native_draw_state(&p, &color, &depth, &area, 640, 480, &out) != VK_SUCCESS && !out.cx_count);
    pair.ready = 1; color.registers[0].offset = 0;
    assert(ps5vk_native_draw_state(&p, &color, &depth, &area, 640, 480, &out) != VK_SUCCESS);
    color.registers[0].offset=offsets[0];pair.vertex_quantization=0;
    assert(ps5vk_native_draw_state(&p,&color,&depth,&area,640,480,&out)!=VK_SUCCESS && !out.cx_count);
}

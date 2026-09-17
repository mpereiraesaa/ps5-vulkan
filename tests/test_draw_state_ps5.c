#include "draw_state_ps5.h"
#include <assert.h>
#include <math.h>
#include <string.h>
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }
/* The polygon-offset block follows PA_SU_VTX_CNTL (0x2f9) at `at`. */
static void check_polygon_offset(const struct ps5vk_draw_state *out, unsigned at,
    int depth, float clamp, float slope, float constant)
{
    assert(out->cx[at].offset==0x2de &&
        out->cx[at].value==(depth ? ((uint32_t)(-23) & 0xffu) | (1u << 8) : 0u));
    assert(out->cx[at+1].offset==0x2df && out->cx[at+1].value==bits(clamp));
    assert(out->cx[at+2].offset==0x2e0 && out->cx[at+2].value==bits(slope*16.0f));
    assert(out->cx[at+3].offset==0x2e1 && out->cx[at+3].value==bits(constant));
    assert(out->cx[at+4].offset==0x2e2 && out->cx[at+4].value==bits(slope*16.0f));
    assert(out->cx[at+5].offset==0x2e3 && out->cx[at+5].value==bits(constant));
}
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
    /* The snapshot a draw carries: the disabled form first. */
    struct ps5vk_raster_state raster = {0};
    static const uint32_t offsets[16] = {0x318,0x31b,0x31c,0x31d,0x31e,0x31f,0x321,0x323,
        0x324,0x325,0x390,0x398,0x3a0,0x3a8,0x3b0,0x3b8};
    struct ps5vk_target_registers color = {.count = 16}, depth = {.count = PS5_DEPTH_REGISTER_COUNT};
    for (unsigned i = 0; i < 16; ++i) color.registers[i].offset = offsets[i];
    VkRect2D area = {{0,0},{640,480}};
    struct ps5vk_draw_state out;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, NULL, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx_count == 98 && out.modifier == 5);
    assert(out.cx[91].offset==0x2f9 && out.cx[91].value==0x2d);
    assert(out.cx[90].offset==0x204 && out.cx[90].value==0x01080000);
    assert(out.cx[87].offset==0x292 && out.cx[87].value==0x22);
    assert(out.cx[88].offset==0x094 && out.cx[88].value==0x80000000u);
    assert(out.cx[89].offset==0x095 && out.cx[89].value==(640u|(480u<<16)));
    assert(out.sh[2].value == 0x123456 && out.sh[10].value == 0xabcdef);
    assert(out.cx[84].value == 0 && out.cx[85].value == 0x246);
    assert(out.cx[86].offset==0x206 && out.cx[86].value==0x43f); /* homogeneous W, not reciprocal */
    /* Disabled bias: no POLY_OFFSET enable bits, zero factors, no depth format. */
    check_polygon_offset(&out, 92, 0, 0.0f, 0.0f, 0.0f);
    float scale; memcpy(&scale, &out.cx[18].value, sizeof(scale)); assert(scale == 240);
    /* A missing snapshot is a caller bug, not a default. */
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,NULL,&color,NULL,&area,640,480,&out)!=VK_SUCCESS && !out.cx_count);
    p.scissor=(VkRect2D){{100,50},{128,128}};
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&raster,&color,NULL,&area,640,480,&out)==VK_SUCCESS);
    assert(out.cx[28].offset==0x090 && out.cx[28].value==0x80000000u);
    assert(out.cx[29].offset==0x091 && out.cx[29].value==(640u|(480u<<16)));
    assert(out.cx[88].value==(0x80000000u|100u|(50u<<16)));
    assert(out.cx[89].value==(228u|(178u<<16)));
    /* Enabled ZERO bias without a depth attachment: the enable bits are set
     * (that is the state the application asked for), the factors stay zero,
     * and the format word is zero because there is no depth representation. */
    raster.depth_bias_enable=VK_TRUE;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&raster,&color,NULL,&area,640,480,&out)==VK_SUCCESS);
    assert(out.cx[85].value == (0x246u | (7u << 11)));
    check_polygon_offset(&out, 92, 0, 0.0f, 0.0f, 0.0f);
    raster.depth_bias_enable=VK_FALSE;
    p.depth_format = VK_FORMAT_D32_SFLOAT; p.depth_test = p.depth_write = VK_TRUE;
    p.depth_compare = VK_COMPARE_OP_LESS;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, &depth, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx_count == 120 && out.cx_count<=PS5VK_DRAW_CX_CAPACITY && out.cx[106].value == 0x16);
    assert(out.cx[112].offset==0x204 && out.cx[112].value==0x01080000);
    assert(out.cx[113].offset==0x2f9 && out.cx[113].value==0x2d);
    /* With a D32 attachment the format word describes the float depth even
     * while the bias is disabled; only the factors and enables say "off". */
    check_polygon_offset(&out, 114, 1, 0.0f, 0.0f, 0.0f);
    assert(out.cx[107].offset==0x205 && out.cx[107].value==0x246u);
    /* Real factors: slope is scaled by 16 for the hardware, the constant and
     * the clamp are carried as-is, and front/back share the polygon's bias.
     * Negative and NaN values are bit patterns, not rejected inputs. */
    raster=(struct ps5vk_raster_state){.depth_bias_enable=VK_TRUE,
        .depth_bias_constant=-3.5f,.depth_bias_clamp=-0.003f,.depth_bias_slope=1.25f};
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, &depth, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx[107].value==(0x246u | (7u << 11)));
    check_polygon_offset(&out, 114, 1, -0.003f, 1.25f, -3.5f);
    raster.depth_bias_clamp=NAN;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, &depth, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx[115].value==bits(raster.depth_bias_clamp));
    /* depthClampEnable disables the near and far z clip planes in
     * PA_CL_CLIP_CNTL and leaves everything else in the word alone; the clamp
     * interval is the viewport's ordered depth range in ZMIN/ZMAX (0x0b4/0x0b5),
     * which a reversed viewport keeps ordered. */
    raster.depth_clamp=VK_TRUE;
    p.viewport=(VkViewport){0,0,640,480,0.9f,0.1f};
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, &depth, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx[112].offset==0x204 && out.cx[112].value==(0x01080000u | (1u<<26) | (1u<<27)));
    assert(out.cx[22].offset==0x0b4 && out.cx[22].value==bits(0.1f));
    assert(out.cx[23].offset==0x0b5 && out.cx[23].value==bits(0.9f));
    assert(out.cx[20].offset==0x113 && out.cx[20].value==bits(0.1f-0.9f));
    assert(out.cx[21].offset==0x114 && out.cx[21].value==bits(0.9f));
    raster.depth_clamp=VK_FALSE;
    p.viewport=(VkViewport){0,0,640,480,0,1};
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, &depth, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx[112].value==0x01080000u);
    raster=(struct ps5vk_raster_state){0};
    p.depth_test = VK_FALSE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, &depth, &area, 640, 480, &out) == VK_SUCCESS);
    assert(out.cx[106].value == 0);
    pair.ready = 0;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, &depth, &area, 640, 480, &out) != VK_SUCCESS && !out.cx_count);
    pair.ready = 1; color.registers[0].offset = 0;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &raster, &color, &depth, &area, 640, 480, &out) != VK_SUCCESS);
    color.registers[0].offset=offsets[0];pair.vertex_quantization=0;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&raster,&color,&depth,&area,640,480,&out)!=VK_SUCCESS && !out.cx_count);
    pair.vertex_quantization=0x2d;
    pair.runtime_arguments=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=2,
        .fragment_count=2,.base_vertex_slot=0,.start_instance_slot=UINT32_MAX,.lds_slot=1,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    pair.runtime_vertex.header.num_cx_registers=11;
    pair.runtime_fragment.header.num_cx_registers=9;
    pair.runtime_vertex.header.num_sh_registers=6;
    pair.runtime_fragment.header.num_sh_registers=4;
    pair.runtime_vertex.context[10]=(ps5_agc_register){0x2ab,1};
    pair.runtime_vertex.shader[5]=(ps5_agc_register){0x81,0xffff};
    pair.runtime_fragment.shader[3]=(ps5_agc_register){0xb,4};
    pair.runtime_vertex.specials.draw_modifier=7;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&raster,&color,&depth,&area,640,480,&out)==VK_SUCCESS);
    assert(out.cx_count==121 && out.cx[75].offset==0x2ab && out.cx[75].value==1);
    assert(out.sh_count==10 && out.sh[5].offset==0x81 && out.sh[9].offset==0xb);
    assert(out.modifier==5 && out.runtime.enabled && out.runtime.base_vertex_slot==0);
    /* The runtime path appends the same polygon-offset block after 0x2f9. */
    assert(out.cx[114].offset==0x2f9 && out.cx[115].offset==0x2de && out.cx[120].offset==0x2e3);
    assert(out.cx_count<=PS5VK_DRAW_CX_CAPACITY);
}

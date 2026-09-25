#include "draw_state_ps5.h"
#include "blend_ps5.h"
#include <assert.h>
#include <math.h>
#include <string.h>
static uint32_t last_cx(const struct ps5vk_draw_state *s,uint32_t offset)
{
    for(unsigned i=s->cx_count;i>0;--i)
        if(s->cx[i-1].offset==offset)return s->cx[i-1].value;
    assert(!"missing context register");return 0;
}
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
    /* The three SX words are GLOBAL registers with per-MRT fields, so a second
     * target's conversion is composed into the same words: each target's state
     * lands in its own field, and one target reproduces the values the profile
     * has always emitted. */
    {
        const uint32_t per_target[PS5VK_MAX_COLOR_ATTACHMENTS][3] = {{5, 6, 0}, {5, 6, 0}};
        uint32_t words[3];
        ps5vk_color_export_compose(per_target, 1, words);
        assert(words[0] == 5u && words[1] == 6u && words[2] == 0u);
        ps5vk_color_export_compose(per_target, PS5VK_MAX_COLOR_ATTACHMENTS, words);
        assert(words[0] == 0x55u && words[1] == 0x66u && words[2] == 0u);
    }

    /* Register-only fixture. No native shader objects or GPU execution. */
    struct VkDevice_T device = {0};
    struct ps5vk_graphics_pair pair = {.ready = 1, .vertex_quantization=0x2d};
    pair.gs.specials.draw_modifier = 5;
    pair.gs.sh[2] = (ps5_agc_register){0x8a, 0x123456};
    pair.ps.sh[4] = (ps5_agc_register){0xa, 0xabcdef};
    struct ps5vk_native_graphics_pipeline native = {.device = &device, .pair = &pair};
    struct VkPipeline_T p = {.device = &device, .graphics = 1, .graphics_state = &native,
        .viewport_count=1, .viewport={0,0,640,480,0,1}, .scissor = {{0,0},{640,480}},
        .color_format = {VK_FORMAT_B8G8R8A8_UNORM}, .color_attachment_count = 1, .cull_mode = VK_CULL_MODE_BACK_BIT,
        .front_face = VK_FRONT_FACE_CLOCKWISE};
    /* The snapshot a draw carries: the disabled form first. */
    struct ps5vk_raster_state raster = {0};
    static const uint32_t offsets[16] = {0x318,0x31b,0x31c,0x31d,0x31e,0x31f,0x321,0x323,
        0x324,0x325,0x390,0x398,0x3a0,0x3a8,0x3b0,0x3b8};
    struct ps5vk_target_registers color = {.count = 16}, depth = {.count = PS5_DEPTH_REGISTER_COUNT};
    for (unsigned i = 0; i < 16; ++i) color.registers[i].offset = offsets[i];
    VkRect2D area = {{0,0},{640,480}};
    struct ps5vk_draw_state out;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, NULL, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx_count == 109 && out.modifier == 5 && out.uc_count == 4);
    /* Primitive restart (DXVK262-T04): the reset index is written after the
     * raster block on every draw and stays clear for a pipeline that never
     * asked to cut, together with the user-config enable. */
    assert(out.cx[102].offset==0x103 && out.cx[102].value==0);
    assert(last_cx(&out,0x1e0)==0 && last_cx(&out,0x1d8)==0x00770077u);
    for(unsigned i=0;i<4;++i)assert(last_cx(&out,0x105+i)==0);
    p.color_blend[0]=(VkPipelineColorBlendAttachmentState){.blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE};
    p.blend_constants[0]=0.5f;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,NULL,&area,640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x1e0)==0x40000104u && last_cx(&out,0x105)==0x3f000000u);
    p.color_blend[0].srcColorBlendFactor=VK_BLEND_FACTOR_SRC1_COLOR;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,NULL,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    pair.dual_source_export=1;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,NULL,&area,640,480,0,&out)==VK_ERROR_UNKNOWN && !out.cx_count);
    pair.dual_source_export=0;
    p.color_blend[0].blendEnable=VK_FALSE;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,NULL,&area,640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x1e0)==0 && last_cx(&out,0x105)==0);
    p.color_blend[0]=(VkPipelineColorBlendAttachmentState){0};p.blend_constants[0]=0;
    assert(out.uc[3].offset==0x24b && out.uc[3].value==0);
    /* Point/line rasterization words follow the polygon offset block. */
    assert(out.cx[98].offset==0x280 && out.cx[98].value==((8u<<16)|8u));
    assert(out.cx[99].offset==0x281 && out.cx[99].value==(0xffffu<<16));
    assert(out.cx[100].offset==0x282 && out.cx[100].value==8u);
    assert(out.cx[101].offset==0x2f7 && out.cx[101].value==0u);
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
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,NULL,&color,1,NULL,&area,640,480, 0, &out)!=VK_SUCCESS && !out.cx_count);
    p.scissor=(VkRect2D){{100,50},{128,128}};
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,NULL,&area,640,480, 0, &out)==VK_SUCCESS);
    assert(out.cx[28].offset==0x090 && out.cx[28].value==0x80000000u);
    assert(out.cx[29].offset==0x091 && out.cx[29].value==(640u|(480u<<16)));
    assert(out.cx[88].value==(0x80000000u|100u|(50u<<16)));
    assert(out.cx[89].value==(228u|(178u<<16)));
    /* Enabled ZERO bias without a depth attachment: the enable bits are set
     * (that is the state the application asked for), the factors stay zero,
     * and the format word is zero because there is no depth representation. */
    raster.depth_bias_enable=VK_TRUE;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,NULL,&area,640,480, 0, &out)==VK_SUCCESS);
    assert(out.cx[85].value == (0x246u | (7u << 11)));
    check_polygon_offset(&out, 92, 0, 0.0f, 0.0f, 0.0f);
    raster.depth_bias_enable=VK_FALSE;
    p.depth_format = VK_FORMAT_D32_SFLOAT; p.depth_test = p.depth_write = VK_TRUE;
    p.depth_compare = VK_COMPARE_OP_LESS;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx_count == 131 && out.cx_count<=PS5VK_DRAW_CX_CAPACITY && out.cx[106].value == 0x16);
    assert(out.cx[112].offset==0x204 && out.cx[112].value==0x01080000);
    assert(out.cx[113].offset==0x2f9 && out.cx[113].value==0x2d);
    /* With a D32 attachment the format word describes the float depth even
     * while the bias is disabled; only the factors and enables say "off". */
    check_polygon_offset(&out, 114, 1, 0.0f, 0.0f, 0.0f);
    /* The precise-query CTS leaf uses this exact 128x128 D16 target extent. */
    {
        struct VkPipeline_T d16 = p;
        struct ps5vk_draw_state d16_state;
        d16.depth_format=VK_FORMAT_D16_UNORM;
        d16.color_format[0]=VK_FORMAT_R8G8B8A8_UNORM;
        raster.depth_bias_enable=VK_TRUE;
        assert(ps5vk_native_draw_state(&d16,&d16.viewport,&d16.scissor,1,
            &raster,&color,1,&depth,&area,128,128,0,&d16_state)==VK_SUCCESS);
        assert(d16_state.cx_count==out.cx_count && d16_state.cx[112].offset==0x204);
        assert(last_cx(&d16_state,0x2de)==0xf0u);
        raster.depth_bias_constant=1.0f;
        assert(ps5vk_native_draw_state(&d16,&d16.viewport,&d16.scissor,1,
            &raster,&color,1,&depth,&area,128,128,0,&d16_state)==VK_ERROR_FEATURE_NOT_PRESENT);
        raster.depth_bias_constant=0.0f;
        assert(ps5vk_native_draw_state(&d16,&d16.viewport,&d16.scissor,1,
            &raster,&color,1,&depth,&area,127,128,0,&d16_state)==VK_ERROR_FEATURE_NOT_PRESENT);
        raster.depth_bias_enable=VK_FALSE;
    }
    assert(out.cx[107].offset==0x205 && out.cx[107].value==0x246u);
    /* Real factors: slope is scaled by 16 for the hardware, the constant and
     * the clamp are carried as-is, and front/back share the polygon's bias.
     * Negative and NaN values are bit patterns, not rejected inputs. */
    raster=(struct ps5vk_raster_state){.depth_bias_enable=VK_TRUE,
        .depth_bias_constant=-3.5f,.depth_bias_clamp=-0.003f,.depth_bias_slope=1.25f};
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[107].value==(0x246u | (7u << 11)));
    check_polygon_offset(&out, 114, 1, -0.003f, 1.25f, -3.5f);
    raster.depth_bias_clamp=NAN;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[115].value==bits(raster.depth_bias_clamp));
    /* depthClampEnable disables the near and far z clip planes in
     * PA_CL_CLIP_CNTL and leaves everything else in the word alone; the clamp
     * interval is the viewport's ordered depth range in ZMIN/ZMAX (0x0b4/0x0b5),
     * which a reversed viewport keeps ordered. */
    raster.depth_clamp=VK_TRUE;
    p.viewport=(VkViewport){0,0,640,480,0.9f,0.1f};
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[112].offset==0x204 && out.cx[112].value==(0x01080000u | (1u<<26) | (1u<<27)));
    assert(out.cx[22].offset==0x0b4 && out.cx[22].value==bits(0.1f));
    assert(out.cx[23].offset==0x0b5 && out.cx[23].value==bits(0.9f));
    assert(out.cx[20].offset==0x113 && out.cx[20].value==bits(0.1f-0.9f));
    assert(out.cx[21].offset==0x114 && out.cx[21].value==bits(0.9f));
    raster.depth_clamp=VK_FALSE;
    p.viewport=(VkViewport){0,0,640,480,0,1};
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[112].value==0x01080000u);
    /* Polygon modes in PA_SU_SC_MODE_CNTL: FILL keeps PTYPE = triangles with
     * POLY_MODE clear; LINE and POINT set PTYPE for both faces, POLY_MODE
     * (bit 3) and KEEP_TOGETHER_ENABLE (bit 24), and leave cull/face alone.
     * A mode outside the three core values is refused, not mapped. */
    raster.depth_bias_enable=VK_FALSE;
    raster.polygon_mode=VK_POLYGON_MODE_LINE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[107].offset==0x205 && out.cx[107].value==0x0100012eu);
    /* The offset enables and the polygon mode are independent bits. */
    raster.depth_bias_enable=VK_TRUE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[107].value==(0x0100012eu | (7u << 11)));
    raster.depth_bias_enable=VK_FALSE;
    raster.polygon_mode=VK_POLYGON_MODE_POINT;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[107].value==0x0100000eu);
    raster.polygon_mode=VK_POLYGON_MODE_FILL_RECTANGLE_NV;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) != VK_SUCCESS && !out.cx_count);
    raster.polygon_mode=VK_POLYGON_MODE_FILL;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[107].value==0x246u);
    raster=(struct ps5vk_raster_state){0};
    p.depth_test = VK_FALSE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) == VK_SUCCESS);
    assert(out.cx[106].value == 0);
    pair.ready = 0;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) != VK_SUCCESS && !out.cx_count);
    pair.ready = 1; color.registers[0].offset = 0;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, 1, &raster, &color, 1, &depth, &area, 640, 480,  0, &out) != VK_SUCCESS);
    color.registers[0].offset=offsets[0];pair.vertex_quantization=0;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480, 0, &out)!=VK_SUCCESS && !out.cx_count);
    pair.vertex_quantization=0x2d;
    pair.runtime_arguments=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=2,
        .fragment_count=2,.base_vertex_slot=0,.start_instance_slot=UINT32_MAX,.lds_slot=1,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    pair.runtime_vertex.header.num_cx_registers=11;
    pair.runtime_fragment.header.num_cx_registers=9;
    p.color_format[0]=VK_FORMAT_R8G8B8A8_UNORM;
    pair.runtime_fragment.context[0]=(ps5_agc_register){0x1c5,9};
    pair.runtime_fragment.context[1]=(ps5_agc_register){0x08f,15};
    pair.runtime_vertex.header.num_sh_registers=6;
    pair.runtime_fragment.header.num_sh_registers=4;
    pair.runtime_vertex.context[10]=(ps5_agc_register){0x2ab,1};
    pair.runtime_vertex.shader[5]=(ps5_agc_register){0x81,0xffff};
    pair.runtime_fragment.shader[3]=(ps5_agc_register){0xb,4};
    pair.runtime_vertex.specials.draw_modifier=7;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    assert(out.cx_count==135 && out.cx[75].offset==0x2ab && out.cx[75].value==1);
    assert(out.sh_count==10 && out.sh[5].offset==0x81 && out.sh[9].offset==0xb);
    assert(out.modifier==5 && out.runtime.enabled && out.runtime.base_vertex_slot==0);
    assert(last_cx(&out,0x1d5)==1 && last_cx(&out,0x1d6)==0 && last_cx(&out,0x1d7)==0);
    p.color_blend[0]=(VkPipelineColorBlendAttachmentState){.blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE};
    /* Reject the mismatched export; changing only blend factors is not enough. */
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    pair.runtime_fragment.context[0].value=4;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x1d5)==5 && last_cx(&out,0x1d6)==6 && last_cx(&out,0x1d7)==0);
    /* The pair flag and exact compiler registers must agree.  Neither side on
     * its own can authorize a dual-source draw. */
    p.color_blend[0].srcColorBlendFactor=VK_BLEND_FACTOR_SRC1_COLOR;
    pair.dual_source_export=1;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    pair.runtime_fragment.context[0].value=0x44;
    pair.runtime_fragment.context[1].value=0xff;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x1e0)==0x6104010fu);
    assert(last_cx(&out,0x1d5)==5 && last_cx(&out,0x1d6)==6);
    pair.dual_source_export=0;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    pair.runtime_fragment.context[0].value=4;
    pair.runtime_fragment.context[1].value=15;
    p.color_blend[0].srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
    p.color_blend[0].blendEnable=VK_FALSE;
    pair.runtime_fragment.context[0].value=9;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x1e0)==0 && last_cx(&out,0x1d5)==1 && last_cx(&out,0x1d6)==0 && last_cx(&out,0x1d7)==0);
    /* A fragment stage with an SSBO side effect before OpKill can have no
     * reachable colour export.  The compiler contract is the paired zero
     * values, not either register in isolation; blending remains impossible
     * without a source export. */
    pair.runtime_fragment.context[0].value=0;
    pair.runtime_fragment.context[1].value=0;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x1d5)==0 && last_cx(&out,0x1d6)==0 && last_cx(&out,0x1d7)==0);
    pair.runtime_fragment.context[1].value=15;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    pair.runtime_fragment.context[0].value=9;
    pair.runtime_fragment.context[1].value=0;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    pair.runtime_fragment.context[0].value=0;
    p.color_blend[0].blendEnable=VK_TRUE;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    p.color_blend[0].blendEnable=VK_FALSE;
    pair.runtime_fragment.context[0].value=9;
    pair.runtime_fragment.context[1].value=15;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    /* The runtime path appends the same polygon-offset block after 0x2f9. */
    assert(out.cx[114].offset==0x2f9 && out.cx[115].offset==0x2de && out.cx[120].offset==0x2e3);
    assert(out.cx[121].offset==0x280 && out.cx[124].offset==0x2f7);
    assert(out.cx_count<=PS5VK_DRAW_CX_CAPACITY);
    /* Viewport arrays: index zero still lands in the base block, every further
     * index appends its own ten-word bank after the point/line words, each
     * with its own scissor/render-area intersection; the count is bounded by
     * the pipeline capacity and a zero count is a caller bug. */
    VkViewport viewports[PS5VK_MAX_VIEWPORTS]; VkRect2D scissors[PS5VK_MAX_VIEWPORTS];
    for(unsigned i=0;i<PS5VK_MAX_VIEWPORTS;++i) {
        viewports[i]=(VkViewport){(float)(i*40),(float)(i*30),40,30,0,1};
        scissors[i]=(VkRect2D){{(int32_t)(i*40),(int32_t)(i*30)},{40,30}};
    }
    assert(ps5vk_native_draw_state(&p,viewports,scissors,3,&raster,&color,1,&depth,&area,640,480, 0, &out)==VK_SUCCESS);
    assert(out.cx_count==135+20 && out.cx_count<=PS5VK_DRAW_CX_CAPACITY);
    assert(out.cx[16].offset==0x10f && bits(20.0f)==out.cx[16].value); /* bank 0 in place */
    assert(out.cx[111].offset==0x094 && out.cx[111].value==0x80000000u &&
        out.cx[112].value==(40u|(30u<<16)));
    assert(out.cx[125].offset==0x115 && out.cx[125].value==bits(20.0f) &&
        out.cx[126].value==bits(60.0f) && out.cx[128].value==bits(45.0f));
    assert(out.cx[131].offset==0x0b6 && out.cx[133].offset==0x096 &&
        out.cx[133].value==(0x80000000u|40u|(30u<<16)) && out.cx[134].value==(80u|(60u<<16)));
    assert(out.cx[135].offset==0x11b && out.cx[143].offset==0x098 &&
        out.cx[143].value==(0x80000000u|80u|(60u<<16)) && out.cx[144].value==(120u|(90u<<16)));
    /* The last bank the pipeline can name, and the capacity it needs. */
    assert(ps5vk_native_draw_state(&p,viewports,scissors,PS5VK_MAX_VIEWPORTS,&raster,&color,1,&depth,&area,640,480, 0, &out)==VK_SUCCESS);
    assert(out.cx_count==135+15*10 && out.cx_count<=PS5VK_DRAW_CX_CAPACITY);
    /* The primitive-restart index is the one register written after the last
     * bank, so the bank's ten words end one slot before the tail. */
    assert(out.cx[out.cx_count-10].offset==0x103 && out.cx[out.cx_count-20].offset==0x169 &&
        out.cx[out.cx_count-12].offset==0x0b2 && out.cx[out.cx_count-11].offset==0x0b3);
    /* A bank whose scissor lies outside the render area clips everything. */
    scissors[5]=(VkRect2D){{2000,2000},{8,8}};
    assert(ps5vk_native_draw_state(&p,viewports,scissors,6,&raster,&color,1,&depth,&area,640,480, 0, &out)==VK_SUCCESS);
    assert(out.cx[125+40+8].offset==0x09e && (out.cx[125+40+8].value&0x7fff)==(out.cx[125+40+9].value&0x7fff));
    assert(ps5vk_native_draw_state(&p,viewports,scissors,0,&raster,&color,1,&depth,&area,640,480, 0, &out)!=VK_SUCCESS && !out.cx_count);
    assert(ps5vk_native_draw_state(&p,viewports,scissors,PS5VK_MAX_VIEWPORTS+1,&raster,&color,1,&depth,&area,640,480, 0, &out)!=VK_SUCCESS && !out.cx_count);
    /* An invalid element anywhere in the array fails the whole state. */
    viewports[2].height=NAN;
    assert(ps5vk_native_draw_state(&p,viewports,scissors,3,&raster,&color,1,&depth,&area,640,480, 0, &out)!=VK_SUCCESS);
    /* Primitive restart (DXVK262-T04, preserved across the raster/viewport
     * merge): the pipeline declares the state and the draw's index width
     * decides the reset value the front end compares against. The pair is
     * written after the raster block and the viewport banks, so the two
     * registers are the last ones the draw programs. */
    raster=(struct ps5vk_raster_state){0};
    struct VkPipeline_T restart_pipeline = p;
    restart_pipeline.primitive_restart = VK_TRUE;
    assert(ps5vk_native_draw_state(&restart_pipeline,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,
        640,480,2,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x103)==0xffffu);
    assert(last_cx(&out,0x2a4)==38u);
    assert(out.uc[3].offset==0x24b && out.uc[3].value==1u);
    assert(ps5vk_native_draw_state(&restart_pipeline,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,
        640,480,4,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x103)==0xffffffffu && out.uc[3].value==1u);
    /* A non-indexed draw cannot carry a restart index, so the enable stays clear
     * even on a pipeline that declared the state. */
    assert(ps5vk_native_draw_state(&restart_pipeline,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,
        640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x103)==0u);
    assert(out.uc[3].value==0u);
    /* An index width neither 2 nor 4 is not a form this path can program. */
    assert(ps5vk_native_draw_state(&restart_pipeline,&p.viewport,&p.scissor,1,&raster,&color,1,&depth,&area,
        640,480,3,&out)!=VK_SUCCESS && !out.cx_count);
    /* The restart pair also survives a multi-viewport draw: the banks are
     * written between the raster block and the pair, never over it. */
    assert(ps5vk_native_draw_state(&restart_pipeline,viewports,scissors,2,&raster,&color,1,&depth,&area,
        640,480,2,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x103)==0xffffu && last_cx(&out,0x2a4)==38u);

    /* DEPTH-ONLY: no colour target at all. The pipeline records a colour count
     * of zero and an undefined colour format, and the state builds against a
     * depth target alone. */
    {
        /* Its own pair: a depth-only pixel program exports nothing, so
         * SPI_SHADER_COL_FORMAT (0x1c5) reads zero rather than the colour
         * export the pair above carries. */
        struct ps5vk_graphics_pair depth_pair = pair;
        depth_pair.runtime_fragment.context[0] = (ps5_agc_register){0x1c5, 0};
        struct ps5vk_native_graphics_pipeline depth_native = native;
        depth_native.pair = &depth_pair;
        struct VkPipeline_T depth_only = p;
        depth_only.graphics_state = &depth_native;
        depth_only.color_format[0] = VK_FORMAT_UNDEFINED;
        depth_only.color_attachment_count = 0;
        depth_only.depth_format = VK_FORMAT_D32_SFLOAT;
        depth_only.color_blend[0] = (VkPipelineColorBlendAttachmentState){0};
        struct ps5vk_draw_state only;
        assert(ps5vk_native_draw_state(&depth_only,&depth_only.viewport,&depth_only.scissor,1,
            &raster,NULL,0,&depth,&area,640,480,0,&only)==VK_SUCCESS);
        /* The colour block is still emitted, and every one of its registers
         * reads zero: CB_COLOR0_INFO.FORMAT (0x31c) is COLOR_INVALID, and
         * CB_TARGET_MASK (0x08e) writes no channel. */
        for (unsigned i = 0; i < 16; ++i) {
            assert(only.cx[i].offset == offsets[i]);
            assert(only.cx[i].value == 0u);
        }
        assert(last_cx(&only,0x08e)==0u);

        /* The two facts travel together in both directions: a colour target
         * handed to a depth-only pipeline, and a colour pipeline given no
         * target at all, are both refused. */
        struct ps5vk_draw_state rejected;
        assert(ps5vk_native_draw_state(&depth_only,&depth_only.viewport,&depth_only.scissor,1,
            &raster,&color,1,&depth,&area,640,480,0,&rejected)!=VK_SUCCESS && !rejected.cx_count);
        struct VkPipeline_T coloured = p;
        coloured.graphics_state = &depth_native;
        coloured.depth_format = VK_FORMAT_D32_SFLOAT;
        assert(ps5vk_native_draw_state(&coloured,&coloured.viewport,&coloured.scissor,1,
            &raster,NULL,0,&depth,&area,640,480,0,&rejected)!=VK_SUCCESS && !rejected.cx_count);
        /* A depth-only draw still needs a depth target. */
        assert(ps5vk_native_draw_state(&depth_only,&depth_only.viewport,&depth_only.scissor,1,
            &raster,NULL,0,NULL,&area,640,480,0,&rejected)!=VK_SUCCESS && !rejected.cx_count);

        /* T09: the stencil test on the combined D32_SFLOAT_S8_UINT target.
         * Without the test the draw is the D32 draw word for word (the float
         * polygon-offset format included); with it, DB_DEPTH_CONTROL gains
         * STENCIL_ENABLE, BACKFACE_ENABLE and both compare functions, and the
         * three stencil words follow it. */
        struct VkPipeline_T ds = depth_only;
        ds.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        struct ps5vk_raster_state stencil_raster = raster;
        stencil_raster.stencil_test = VK_FALSE;
        struct ps5vk_draw_state plain, stencilled;
        assert(ps5vk_native_draw_state(&depth_only,&depth_only.viewport,&depth_only.scissor,1,
            &stencil_raster,NULL,0,&depth,&area,640,480,0,&only)==VK_SUCCESS);
        assert(ps5vk_native_draw_state(&ds,&ds.viewport,&ds.scissor,1,
            &stencil_raster,NULL,0,&depth,&area,640,480,0,&plain)==VK_SUCCESS);
        assert(plain.cx_count==only.cx_count &&
               !memcmp(plain.cx,only.cx,plain.cx_count*sizeof(plain.cx[0])));
        stencil_raster.stencil_test = VK_TRUE;
        stencil_raster.stencil_front = (VkStencilOpState){VK_STENCIL_OP_ZERO,
            VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_COMPARE_OP_EQUAL,
            0x0f, 0xf0, 0x5a};
        stencil_raster.stencil_back = (VkStencilOpState){VK_STENCIL_OP_INVERT,
            VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_GREATER,
            0x1ff, 0x3ff, 0x1a5};
        /* The depth-only target refuses the stencil test: no stencil plane. */
        assert(ps5vk_native_draw_state(&depth_only,&depth_only.viewport,&depth_only.scissor,1,
            &stencil_raster,NULL,0,&depth,&area,640,480,0,&rejected)!=VK_SUCCESS &&
            !rejected.cx_count);
        assert(ps5vk_native_draw_state(&ds,&ds.viewport,&ds.scissor,1,
            &stencil_raster,NULL,0,&depth,&area,640,480,0,&stencilled)==VK_SUCCESS);
        assert(stencilled.cx_count==plain.cx_count+3u &&
               stencilled.cx_count<=PS5VK_DRAW_CX_CAPACITY);
        const uint32_t control=last_cx(&plain,0x200);
        assert(last_cx(&stencilled,0x200)==(control|1u|(1u<<7)|(2u<<8)|(4u<<20)));
        /* ZERO=1, REPLACE_TEST=3, ADD_WRAP=8 | INVERT=7, SUB_CLAMP=6, KEEP=0. */
        assert(last_cx(&stencilled,0x10b)==(1u|(3u<<4)|(8u<<8)|(7u<<12)|(6u<<16)|(0u<<20)));
        assert(last_cx(&stencilled,0x10c)==(0x5au|(0x0fu<<8)|(0xf0u<<16)|(1u<<24)));
        /* Only the low eight bits of an 8-bit stencil aspect's values count. */
        assert(last_cx(&stencilled,0x10d)==(0xa5u|(0xffu<<8)|(0xffu<<16)|(1u<<24)));
        ps5_agc_register words[3];
        VkStencilOpState bad = stencil_raster.stencil_front;
        bad.passOp = (VkStencilOp)8;
        assert(ps5vk_stencil_registers(&bad,&stencil_raster.stencil_back,words));
        bad = stencil_raster.stencil_front; bad.compareOp = (VkCompareOp)8;
        assert(ps5vk_stencil_registers(&stencil_raster.stencil_back,&bad,words));
    }
}

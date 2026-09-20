#include "draw_state_ps5.h"
#include <assert.h>
#include <string.h>
static uint32_t last_cx(const struct ps5vk_draw_state *s,uint32_t offset)
{
    for(unsigned i=s->cx_count;i>0;--i)
        if(s->cx[i-1].offset==offset)return s->cx[i-1].value;
    assert(!"missing context register");return 0;
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
    static const uint32_t offsets[16] = {0x318,0x31b,0x31c,0x31d,0x31e,0x31f,0x321,0x323,
        0x324,0x325,0x390,0x398,0x3a0,0x3a8,0x3b0,0x3b8};
    struct ps5vk_target_registers color = {.count = 16}, depth = {.count = PS5_DEPTH_REGISTER_COUNT};
    for (unsigned i = 0; i < 16; ++i) color.registers[i].offset = offsets[i];
    VkRect2D area = {{0,0},{640,480}};
    struct ps5vk_draw_state out;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &color, NULL, &area, 640, 480, 0, &out) == VK_SUCCESS);
    assert(out.cx_count == 99 && out.modifier == 5 && out.uc_count == 4);
    assert(out.cx[93].offset==0x1e0 && out.cx[93].value==0);
    assert(out.cx[94].offset==0x1d8 && out.cx[94].value==0x00770077u);
    for(unsigned i=0;i<4;++i)assert(out.cx[95+i].offset==0x105+i && !out.cx[95+i].value);
    p.color_blend=(VkPipelineColorBlendAttachmentState){.blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE};
    p.blend_constants[0]=0.5f;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,NULL,&area,640,480,0,&out)==VK_SUCCESS);
    assert(out.cx[93].value==0x40000104u && out.cx[95].value==0x3f000000u);
    p.color_blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC1_COLOR;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,NULL,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    p.color_blend.blendEnable=VK_FALSE;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,NULL,&area,640,480,0,&out)==VK_SUCCESS);
    assert(out.cx[93].value==0 && out.cx[95].value==0);
    p.color_blend=(VkPipelineColorBlendAttachmentState){0};p.blend_constants[0]=0;
    assert(out.cx[91].offset==0x2f9 && out.cx[91].value==0x2d);
    /* Primitive restart: the two registers sit after the vertex quantization and
     * stay clear for a draw this pipeline never asked to cut. */
    assert(out.cx[92].offset==0x103 && out.cx[92].value==0);
    assert(out.uc[3].offset==0x24b && out.uc[3].value==0);
    assert(out.cx[90].offset==0x204 && out.cx[90].value==0x01080000);
    assert(out.cx[87].offset==0x292 && out.cx[87].value==0x22);
    assert(out.cx[88].offset==0x094 && out.cx[88].value==0x80000000u);
    assert(out.cx[89].offset==0x095 && out.cx[89].value==(640u|(480u<<16)));
    assert(out.sh[2].value == 0x123456 && out.sh[10].value == 0xabcdef);
    assert(out.cx[84].value == 0 && out.cx[85].value == 0x246);
    assert(out.cx[86].offset==0x206 && out.cx[86].value==0x43f); /* homogeneous W, not reciprocal */
    float scale; memcpy(&scale, &out.cx[18].value, sizeof(scale)); assert(scale == 240);
    p.scissor=(VkRect2D){{100,50},{128,128}};
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,NULL,&area,640,480,0,&out)==VK_SUCCESS);
    assert(out.cx[28].offset==0x090 && out.cx[28].value==0x80000000u);
    assert(out.cx[29].offset==0x091 && out.cx[29].value==(640u|(480u<<16)));
    assert(out.cx[88].value==(0x80000000u|100u|(50u<<16)));
    assert(out.cx[89].value==(228u|(178u<<16)));
    p.depth_format = VK_FORMAT_D32_SFLOAT; p.depth_test = p.depth_write = VK_TRUE;
    p.depth_compare = VK_COMPARE_OP_LESS;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &color, &depth, &area, 640, 480, 0, &out) == VK_SUCCESS);
    assert(out.cx_count == 121 && out.cx_count<=PS5VK_DRAW_CX_CAPACITY && out.cx[106].value == 0x16);
    assert(out.cx[112].offset==0x204 && out.cx[112].value==0x01080000);
    assert(out.cx[113].offset==0x2f9 && out.cx[113].value==0x2d);
    p.depth_test = VK_FALSE;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &color, &depth, &area, 640, 480, 0, &out) == VK_SUCCESS);
    assert(out.cx[106].value == 0);
    pair.ready = 0;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &color, &depth, &area, 640, 480, 0, &out) != VK_SUCCESS && !out.cx_count);
    pair.ready = 1; color.registers[0].offset = 0;
    assert(ps5vk_native_draw_state(&p, &p.viewport, &p.scissor, &color, &depth, &area, 640, 480, 0, &out) != VK_SUCCESS);
    color.registers[0].offset=offsets[0];pair.vertex_quantization=0;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,&depth,&area,640,480,0,&out)!=VK_SUCCESS && !out.cx_count);
    pair.vertex_quantization=0x2d;
    pair.runtime_arguments=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=2,
        .fragment_count=2,.base_vertex_slot=0,.start_instance_slot=UINT32_MAX,.lds_slot=1,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    pair.runtime_vertex.header.num_cx_registers=11;
    pair.runtime_fragment.header.num_cx_registers=9;
    p.color_format=VK_FORMAT_R8G8B8A8_UNORM;
    pair.runtime_fragment.context[0]=(ps5_agc_register){0x1c5,9};
    pair.runtime_vertex.header.num_sh_registers=6;
    pair.runtime_fragment.header.num_sh_registers=4;
    pair.runtime_vertex.context[10]=(ps5_agc_register){0x2ab,1};
    pair.runtime_vertex.shader[5]=(ps5_agc_register){0x81,0xffff};
    pair.runtime_fragment.shader[3]=(ps5_agc_register){0xb,4};
    pair.runtime_vertex.specials.draw_modifier=7;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    assert(out.cx_count==125 && out.cx[75].offset==0x2ab && out.cx[75].value==1);
    assert(out.sh_count==10 && out.sh[5].offset==0x81 && out.sh[9].offset==0xb);
    assert(out.modifier==5 && out.runtime.enabled && out.runtime.base_vertex_slot==0);
    assert(last_cx(&out,0x1d5)==1 && last_cx(&out,0x1d6)==0 && last_cx(&out,0x1d7)==0);
    p.color_blend=(VkPipelineColorBlendAttachmentState){.blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE};
    /* Reject the mismatched export; changing only blend factors is not enough. */
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,&depth,&area,640,480,0,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out.cx_count);
    pair.runtime_fragment.context[0].value=4;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x1d5)==5 && last_cx(&out,0x1d6)==6 && last_cx(&out,0x1d7)==0);
    p.color_blend.blendEnable=VK_FALSE;
    pair.runtime_fragment.context[0].value=9;
    assert(ps5vk_native_draw_state(&p,&p.viewport,&p.scissor,&color,&depth,&area,640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x1e0)==0 && last_cx(&out,0x1d5)==1 && last_cx(&out,0x1d6)==0 && last_cx(&out,0x1d7)==0);
    /* Primitive restart: the pipeline declares the state and the draw's index
     * width decides the reset value the front end compares against. */
    struct VkPipeline_T restart_pipeline = p;
    restart_pipeline.primitive_restart = VK_TRUE;
    assert(ps5vk_native_draw_state(&restart_pipeline,&p.viewport,&p.scissor,&color,&depth,&area,
        640,480,2,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x103)==0xffffu);
    assert(last_cx(&out,0x2a4)==38u);
    assert(out.uc[3].offset==0x24b && out.uc[3].value==1u);
    assert(ps5vk_native_draw_state(&restart_pipeline,&p.viewport,&p.scissor,&color,&depth,&area,
        640,480,4,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x103)==0xffffffffu && out.uc[3].value==1u);
    /* A non-indexed draw cannot carry a restart index, so the enable stays clear
     * even on a pipeline that declared the state. */
    assert(ps5vk_native_draw_state(&restart_pipeline,&p.viewport,&p.scissor,&color,&depth,&area,
        640,480,0,&out)==VK_SUCCESS);
    assert(last_cx(&out,0x103)==0u);
    assert(out.uc[3].value==0u);
    /* An index width neither 2 nor 4 is not a form this path can program. */
    assert(ps5vk_native_draw_state(&restart_pipeline,&p.viewport,&p.scissor,&color,&depth,&area,
        640,480,3,&out)!=VK_SUCCESS && !out.cx_count);
}

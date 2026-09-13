#include "runtime_shader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint32_t code=0;
    PsbcShaderOutput c={.machine_code=&code,.machine_code_size=4};
    PsbcShaderMetadata *m=&c.metadata;
    m->version=PSBC_SHADER_METADATA_VERSION;m->target=PSBC_TARGET_PS5;
    m->source_stage=PSBC_STAGE_VERTEX;m->hardware_stage=PSBC_HW_STAGE_NGG;
    m->address32_hi=2;m->user_sgpr_count=2;m->ngg_lds_layout_valid=true;
    m->linkage_valid=true;
    m->linkage_ge_cntl.offset=0x25b;m->linkage_stages_en.offset=0x2d5;
    m->linkage_user_vgpr_en.offset=0x262;
    m->context_register_count=11;
    for(unsigned i=0;i<11;++i)m->context_registers[i]=(PsbcRegisterWrite){.offset=0x100+i,.value=i};
    m->context_registers[10].offset=0x2ab;
    m->shader_register_count=4;
    m->shader_registers[0].offset=0xc8;m->shader_registers[1].offset=0xc9;
    m->shader_registers[2].offset=0x8a;m->shader_registers[3].offset=0x8b;
    m->output_semantic_count=1;m->output_semantics[0]=15;
    struct ps5vk_runtime_shader arena,before;
    assert(!ps5vk_runtime_shader_build(&arena,&c));
    assert(arena.header.num_cx_registers==11 && arena.context[10].value==1);
    assert(arena.header.num_output_semantics==1 && arena.outputs[0]==15);
    assert((uintptr_t)&arena.header.output_semantics+(uintptr_t)arena.header.output_semantics==(uintptr_t)arena.outputs);
    before=arena;
#define REJECT(field,value) do { PsbcShaderMetadata saved=*m; m->field=(value); \
    assert(ps5vk_runtime_shader_build(&arena,&c)!=0); \
    assert(!memcmp(&arena,&before,sizeof(arena))); *m=saved; } while(0)
    REJECT(context_register_count,PSBC_MAX_CONTEXT_REGISTERS+1);
    REJECT(shader_register_count,PSBC_MAX_SHADER_REGISTERS+1);
    REJECT(output_semantic_count,PSBC_MAX_SEMANTICS+1);
    REJECT(context_registers[10].offset,m->context_registers[0].offset);
    REJECT(shader_registers[0].value,1);
    REJECT(unresolved_fields,PSBC_UNRESOLVED_AGC_LINKAGE);
    REJECT(descriptor_set0_valid,true);
    REJECT(scratch_size_per_thread,4);
    REJECT(address32_hi,1);
    REJECT(ngg_lds_layout_user_data_dword,2);
    REJECT(linkage_user_vgpr_en.offset,0x25c);
    REJECT(source_stage,PSBC_STAGE_GEOMETRY);
#undef REJECT
    PsbcShaderMetadata fragment={.source_stage=PSBC_STAGE_FRAGMENT,
        .hardware_stage=PSBC_HW_STAGE_PIXEL,.user_sgpr_count=2,
        .input_semantic_count=1,.input_semantics={15}};
    struct ps5vk_runtime_draw_abi abi;
    m->base_vertex_valid=true;m->base_vertex_user_data_dword=0;
    m->ngg_lds_layout_user_data_dword=1;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    uint32_t vertex[16],pixel[16];
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0,vertex,pixel));
    assert(vertex[0]==31 && vertex[1]==0 && pixel[0]==0);
    m->user_sgpr_count=3;m->push_constants_valid=true;
    m->push_constants_user_data_dword=2;m->push_constant_size=12;
    fragment.user_sgpr_count=1;fragment.push_constants_valid=true;
    fragment.push_constants_user_data_dword=0;fragment.push_constant_size=12;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.push_constant_size==12 && abi.vertex_push_slot==2 && abi.fragment_push_slot==0);
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0x123400,vertex,pixel));
    assert(vertex[0]==31 && vertex[2]==0x123400 && pixel[0]==0x123400);
    assert(ps5vk_runtime_draw_values(&abi,31,17,0,0,vertex,pixel));
    m->vertex_buffer_table_valid=true;m->vertex_buffer_table_user_data_dword=1;
    m->ngg_lds_layout_user_data_dword=3;m->user_sgpr_count=4;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.vertex_buffer_valid==1 && abi.vertex_buffer_slot==1);
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0x123410,0x123400,vertex,pixel));
    assert(vertex[1]==0x123410 && vertex[2]==0x123400);
    assert(ps5vk_runtime_draw_values(&abi,31,17,0x123414,0x123400,vertex,pixel));
    m->vertex_buffer_table_valid=false;m->vertex_buffer_table_user_data_dword=0;
    m->ngg_lds_layout_user_data_dword=1;m->user_sgpr_count=2;
    m->user_sgpr_count=2;m->push_constants_valid=false;
    m->push_constants_user_data_dword=0;m->push_constant_size=0;
    fragment.user_sgpr_count=2;fragment.push_constants_valid=false;
    fragment.push_constants_user_data_dword=0;fragment.push_constant_size=0;
    fragment.input_semantics[0]=16;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment.input_semantics[0]=15;m->output_semantics[1]=15;m->output_semantic_count=2;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->output_semantic_count=1;m->ngg_lds_layout_user_data_dword=0;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    puts("Runtime shader header: pass (11 registers, relative semantics, rejection without mutation)");
    return 0;
}

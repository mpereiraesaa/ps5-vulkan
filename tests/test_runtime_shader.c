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
    PsbcShaderMetadata fragment={.version=PSBC_SHADER_METADATA_VERSION,.source_stage=PSBC_STAGE_FRAGMENT,
        .hardware_stage=PSBC_HW_STAGE_PIXEL,.user_sgpr_count=2,
        .input_semantic_count=1,.input_semantics={15}};
    struct ps5vk_runtime_draw_abi abi;
    m->base_vertex_valid=true;m->base_vertex_user_data_dword=0;
    m->ngg_lds_layout_user_data_dword=1;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    uint32_t vertex[16],pixel[16];
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0,0,vertex,pixel));
    assert(vertex[0]==31 && vertex[1]==0 && pixel[0]==0);
    m->user_sgpr_count=3;m->push_constants_valid=true;
    m->push_constants_user_data_dword=2;m->push_constant_size=12;
    fragment.user_sgpr_count=1;fragment.push_constants_valid=true;
    fragment.push_constants_user_data_dword=0;fragment.push_constant_size=12;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.push_constant_size==12 && abi.vertex_push_slot==2 && abi.fragment_push_slot==0);
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0x123400,0,vertex,pixel));
    assert(vertex[0]==31 && vertex[2]==0x123400 && pixel[0]==0x123400);
    assert(ps5vk_runtime_draw_values(&abi,31,17,0,0,0,vertex,pixel));
    m->vertex_buffer_table_valid=true;m->vertex_buffer_table_user_data_dword=1;
    m->vertex_buffer_usage_mask=0x8008;
    m->ngg_lds_layout_user_data_dword=3;m->user_sgpr_count=4;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.vertex_buffer_valid==1 && abi.vertex_buffer_slot==1);
    assert(abi.vertex_buffer_usage_mask==0x8008);
    m->vertex_buffer_per_attribute=true;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->vertex_buffer_per_attribute=false;m->vertex_buffer_usage_mask=0;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->vertex_buffer_usage_mask=0x10000;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->vertex_buffer_usage_mask=0x8008;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0x123410,0x123400,0,vertex,pixel));
    assert(vertex[1]==0x123410 && vertex[2]==0x123400);
    assert(ps5vk_runtime_draw_values(&abi,31,17,0x123414,0x123400,0,vertex,pixel));
    fragment.descriptor_binding_count=1;
    fragment.descriptor_bindings[0]=(PsbcDescriptorBinding){.set=0,.binding=0,
        .type=PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,.array_size=1,.offset=0,.stride=48};
    fragment.descriptor_set0_valid=true;fragment.descriptor_set_valid[0]=true;
    fragment.descriptor_set0_user_data_dword=1;
    fragment.descriptor_set_user_data_dword[0]=1;
    fragment.user_sgpr_count=2;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.fragment_descriptor_valid[0] && abi.fragment_descriptor_slot[0]==1);
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0x123410,0x123400,0x900000,vertex,pixel));
    assert(pixel[1]==0x900000);
    assert(ps5vk_runtime_draw_values(&abi,31,17,0x123410,0x123400,0x900004,vertex,pixel));
    PsbcShaderMetadata saved_fragment=fragment,saved_vertex=*m;
    fragment.descriptor_binding_count=4;fragment.user_sgpr_count=5;
    m->descriptor_binding_count=4;m->user_sgpr_count=8;
    for(unsigned s=0;s<4;++s) {
        fragment.descriptor_bindings[s]=(PsbcDescriptorBinding){.set=s,.binding=7,
            .type=PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,.array_size=24,.offset=32,.stride=48};
        fragment.descriptor_set_valid[s]=true;
        fragment.descriptor_set_user_data_dword[s]=s+1;
        m->descriptor_bindings[s]=(PsbcDescriptorBinding){.set=s,.binding=2,
            .type=PSBC_DESCRIPTOR_UNIFORM_BUFFER,.array_size=2,.offset=0,.stride=16};
        m->descriptor_set_valid[s]=true;m->descriptor_set_user_data_dword[s]=s+4;
    }
    m->descriptor_set0_valid=true;m->descriptor_set0_user_data_dword=4;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    uint32_t tables[4]={0x900000,0xa00000,0xb00000,0xc00000};
    assert(!ps5vk_runtime_draw_values_sets(&abi,31,17,0x123410,0x123400,tables,vertex,pixel));
    for(unsigned s=0;s<4;++s)assert(vertex[s+4]==tables[s] && pixel[s+1]==tables[s]);
    uint32_t saved_vs[16],saved_fs[16];
    memcpy(saved_vs,vertex,sizeof(vertex));memcpy(saved_fs,pixel,sizeof(pixel));
#define BAD_ABI(field,value) do { struct ps5vk_runtime_draw_abi saved=abi;abi.field=(value); \
    assert(ps5vk_runtime_draw_values_sets(&abi,31,17,0x123410,0x123400,tables,vertex,pixel)); \
    assert(!memcmp(saved_vs,vertex,sizeof(vertex)) && !memcmp(saved_fs,pixel,sizeof(pixel)));abi=saved; } while(0)
    BAD_ABI(vertex_descriptor_slot[3],0); /* base vertex collision */
    BAD_ABI(vertex_descriptor_slot[3],1); /* vertex SRD collision */
    BAD_ABI(vertex_descriptor_slot[3],2); /* push collision */
    BAD_ABI(vertex_descriptor_slot[3],3); /* LDS collision */
    BAD_ABI(vertex_descriptor_slot[3],4); /* other set collision */
    BAD_ABI(vertex_descriptor_slot[3],16);
    BAD_ABI(fragment_descriptor_slot[3],0); /* push collision */
    BAD_ABI(fragment_descriptor_slot[3],1); /* other set collision */
    BAD_ABI(fragment_descriptor_valid[3],2);
#undef BAD_ABI
    tables[3]+=4;
    assert(ps5vk_runtime_draw_values_sets(&abi,31,17,0x123410,0x123400,tables,vertex,pixel));
    assert(!memcmp(saved_vs,vertex,sizeof(vertex)) && !memcmp(saved_fs,pixel,sizeof(pixel)));
    fragment.descriptor_bindings[3].set=4;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment.descriptor_bindings[3].set=3;
    fragment.descriptor_bindings[3].array_size=UINT32_MAX;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment=saved_fragment;*m=saved_vertex;
    fragment.descriptor_binding_count=0;fragment.descriptor_set0_valid=false;
    fragment.descriptor_set_valid[0]=false;fragment.descriptor_set0_user_data_dword=0;
    fragment.descriptor_set_user_data_dword[0]=0;
    m->vertex_buffer_table_valid=false;m->vertex_buffer_table_user_data_dword=0;
    m->vertex_buffer_usage_mask=0;
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

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
    /* A slot that is not declared must come with a zero dword: a malformed pair
     * is refused instead of being silently discarded. */
    REJECT(base_vertex_user_data_dword,1);
    REJECT(start_instance_user_data_dword,1);
    REJECT(draw_id_user_data_dword,1);
#undef REJECT
    /* The hardware scales the per-vertex offsets it hands a merged geometry half
     * by VGT_ESGS_RING_ITEMSIZE, and next-gen geometry keeps that register at one
     * so those offsets stay ITEM INDICES: upstream never writes it on the NGG
     * path, the state initialiser sets 1, and the shader carries the item size
     * itself - which is what this compiler's addressing does too. The compiler's
     * metadata instead carries the legacy value (the item size in dwords, 5 for
     * a 20-byte item) and even marks it unresolved for a merged pair, so
     * programming it as emitted scales the offsets twice: the console measured
     * the geometry half reading item 5k where it must read item k, which made
     * only the first vertex of each primitive come back correct. */
    {
        uint32_t merged_code=0;
        PsbcShaderOutput pair={.machine_code=&merged_code,.machine_code_size=4};
        PsbcShaderMetadata *g=&pair.metadata;
        static const unsigned geometry_registers[]={0x1ffu,0x291u,0x29bu,0x2abu,0x2ceu,0x2d3u};
        g->version=PSBC_SHADER_METADATA_VERSION;g->target=PSBC_TARGET_PS5;
        g->source_stage=PSBC_STAGE_GEOMETRY;g->hardware_stage=PSBC_HW_STAGE_NGG;
        g->address32_hi=2;g->user_sgpr_count=2;g->ngg_lds_layout_valid=true;
        g->ngg_lds_layout_user_data_dword=1;g->ngg_lds_layout=1680;
        g->linkage_valid=true;
        g->linkage_ge_cntl.offset=0x25b;g->linkage_stages_en.offset=0x2d5;
        g->linkage_user_vgpr_en.offset=0x262;
        g->user_data_window_base=8;g->esgs_system_sgprs_valid=true;
        g->esgs_gs_tg_info_sgpr=2;g->esgs_merged_wave_info_sgpr=3;
        g->unresolved_fields=PSBC_UNRESOLVED_NGG_ESGS_RING_ITEMSIZE;
        g->context_register_count=sizeof(geometry_registers)/sizeof(geometry_registers[0]);
        for(unsigned i=0;i<g->context_register_count;++i)
            g->context_registers[i]=(PsbcRegisterWrite){.offset=geometry_registers[i],
                .value=geometry_registers[i]==0x2abu?5u:i};
        g->shader_register_count=4;
        g->shader_registers[0].offset=0xc8;g->shader_registers[1].offset=0xc9;
        g->shader_registers[2].offset=0x8a;g->shader_registers[3].offset=0x8b;
        g->output_semantic_count=1;g->output_semantics[0]=15;
        struct ps5vk_runtime_shader merged;
        assert(!ps5vk_runtime_shader_build(&merged,&pair));
        unsigned seen=0;
        for(unsigned i=0;i<merged.header.num_cx_registers;++i) {
            if(merged.context[i].offset!=0x2ab)continue;
            ++seen;
            assert(merged.context[i].value==1);
        }
        assert(seen==1u);
    }
    PsbcShaderMetadata fragment={.version=PSBC_SHADER_METADATA_VERSION,.source_stage=PSBC_STAGE_FRAGMENT,
        .hardware_stage=PSBC_HW_STAGE_PIXEL,.user_sgpr_count=2,
        .input_semantic_count=1,.input_semantics={15}};
    struct ps5vk_runtime_draw_abi abi;
    m->base_vertex_valid=true;m->base_vertex_user_data_dword=0;
    m->ngg_lds_layout_user_data_dword=1;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    uint32_t vertex[16],pixel[16];
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0,0,0,vertex,pixel));
    assert(vertex[0]==31 && vertex[1]==0 && pixel[0]==0);
    /* A merged vertex+geometry pair gates and sizes its halves from two SYSTEM
     * SGPRs, below the driver's user-data window: measured on a merged pair
     * (gs_tg_info 2, merged_wave_info 3, window base 8) and, with the window
     * shifted by one, on the hardware-verified draw-parameter program whose
     * BaseVertex window dword 1 is SGPR 9. The driver records the two indices as
     * a contract check and never writes them, and a metadata that reports them
     * inside the window is refused instead of being written where the shader
     * does not look. */
    m->esgs_system_sgprs_valid=true;
    m->esgs_gs_tg_info_sgpr=2;m->esgs_merged_wave_info_sgpr=3;
    m->user_data_window_base=8;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.window_base==8 && abi.esgs_described==1);
    assert(abi.esgs_gs_tg_info_sgpr==2 && abi.esgs_merged_wave_info_sgpr==3);
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0,0,0,vertex,pixel));
    assert(vertex[0]==31 && vertex[1]==0); /* only the two window dwords */
    m->esgs_merged_wave_info_sgpr=8; /* inside the window: refused */
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->esgs_merged_wave_info_sgpr=2; /* both registers in one slot: refused */
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->esgs_merged_wave_info_sgpr=3;
    /* The draw path repeats the check and refuses before writing either bank. */
    struct ps5vk_runtime_draw_abi window_relative=abi;
    window_relative.esgs_merged_wave_info_sgpr=window_relative.window_base;
    uint32_t kept_vertex[16],kept_pixel[16];
    memcpy(kept_vertex,vertex,sizeof(vertex));memcpy(kept_pixel,pixel,sizeof(pixel));
    assert(ps5vk_runtime_draw_values(&window_relative,31,17,0,0,0,0,vertex,pixel));
    assert(!memcmp(kept_vertex,vertex,sizeof(vertex)) &&
           !memcmp(kept_pixel,pixel,sizeof(pixel)));
    m->user_data_window_base=0; /* no base declared for a merged pair: refused */
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->user_data_window_base=16; /* a window past SGPR 16 is unaddressable */
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->user_data_window_base=8;
    m->esgs_system_sgprs_valid=false;m->user_data_window_base=0;
    /* DrawIndex (metadata v13): the ABI mirrors the compiler's slot, the value
     * written into the block is the sequence index the caller supplies, and a
     * slot that collides with another value or falls outside the declared block
     * is refused instead of overwriting it. */
    m->draw_id_valid=true;m->draw_id_user_data_dword=1; /* LDS collision */
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->draw_id_user_data_dword=2; /* outside the two-dword block */
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->user_sgpr_count=3;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.draw_id_slot==2 && abi.vertex_count==3);
    assert(!ps5vk_runtime_draw_values(&abi,31,17,9,0,0,0,vertex,pixel));
    assert(vertex[0]==31 && vertex[2]==9);
    /* The declared slot is inside the block, so the single-stage packing accepts
     * it; one dword further is outside the block and is refused. */
    assert(!ps5vk_runtime_shader_build(&arena,&c));
    m->draw_id_user_data_dword=3;
    assert(ps5vk_runtime_shader_build(&arena,&c));
    m->draw_id_user_data_dword=2;
    /* Absent with a nonzero dword is malformed on the ABI path too. */
    m->draw_id_valid=false;m->draw_id_user_data_dword=1;
    assert(ps5vk_runtime_shader_build(&arena,&c));
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    m->draw_id_valid=true;m->draw_id_user_data_dword=2;
    /* The fragment stage must not carry a DrawIndex slot, valid or not. */
    fragment.draw_id_valid=true; /* a fragment-stage DrawIndex slot is refused */
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment.draw_id_valid=false;fragment.draw_id_user_data_dword=1;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment.draw_id_user_data_dword=0;
    /* BaseVertex and BaseInstance are vertex-stage built-ins as well, and the
     * same zero-dword rule holds for their absent form. */
    fragment.base_vertex_valid=true;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment.base_vertex_valid=false;fragment.base_vertex_user_data_dword=1;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment.base_vertex_user_data_dword=0;
    fragment.start_instance_valid=true;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment.start_instance_valid=false;fragment.start_instance_user_data_dword=1;
    assert(ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    fragment.start_instance_user_data_dword=0;
    m->draw_id_valid=false;m->draw_id_user_data_dword=0;m->user_sgpr_count=2;
    m->user_sgpr_count=3;m->push_constants_valid=true;
    m->push_constants_user_data_dword=2;m->push_constant_size=12;
    fragment.user_sgpr_count=1;fragment.push_constants_valid=true;
    fragment.push_constants_user_data_dword=0;fragment.push_constant_size=12;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.push_constant_size==12 && abi.vertex_push_slot==2 && abi.fragment_push_slot==0);
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0,0x123400,0,vertex,pixel));
    assert(vertex[0]==31 && vertex[2]==0x123400 && pixel[0]==0x123400);
    assert(ps5vk_runtime_draw_values(&abi,31,17,0,0,0,0,vertex,pixel));
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
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0x123410,0x123400,0,vertex,pixel));
    assert(vertex[1]==0x123410 && vertex[2]==0x123400);
    assert(ps5vk_runtime_draw_values(&abi,31,17,0,0x123414,0x123400,0,vertex,pixel));
    fragment.descriptor_binding_count=1;
    fragment.descriptor_bindings[0]=(PsbcDescriptorBinding){.set=0,.binding=0,
        .type=PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,.array_size=1,.offset=0,.stride=48};
    fragment.descriptor_set0_valid=true;fragment.descriptor_set_valid[0]=true;
    fragment.descriptor_set0_user_data_dword=1;
    fragment.descriptor_set_user_data_dword[0]=1;
    fragment.user_sgpr_count=2;
    assert(!ps5vk_runtime_draw_abi_build(m,&fragment,&abi));
    assert(abi.fragment_descriptor_valid[0] && abi.fragment_descriptor_slot[0]==1);
    assert(!ps5vk_runtime_draw_values(&abi,31,17,0,0x123410,0x123400,0x900000,vertex,pixel));
    assert(pixel[1]==0x900000);
    assert(ps5vk_runtime_draw_values(&abi,31,17,0,0x123410,0x123400,0x900004,vertex,pixel));
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
    assert(!ps5vk_runtime_draw_values_sets(&abi,31,17,0,0,0x123410,0x123400,tables,vertex,pixel));
    for(unsigned s=0;s<4;++s)assert(vertex[s+4]==tables[s] && pixel[s+1]==tables[s]);
    uint32_t saved_vs[16],saved_fs[16];
    memcpy(saved_vs,vertex,sizeof(vertex));memcpy(saved_fs,pixel,sizeof(pixel));
#define BAD_ABI(field,value) do { struct ps5vk_runtime_draw_abi saved=abi;abi.field=(value); \
    assert(ps5vk_runtime_draw_values_sets(&abi,31,17,0,0,0x123410,0x123400,tables,vertex,pixel)); \
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
    assert(ps5vk_runtime_draw_values_sets(&abi,31,17,0,0,0x123410,0x123400,tables,vertex,pixel));
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

    /* ViewIndex (metadata v14). Each case starts from the same known-good
     * metadata, so a rejection below cannot be an artefact of the previous
     * case. The vertex metadata declares the slot only when the stage really
     * reads gl_ViewIndex, the ABI mirrors it verbatim, and the value the draw
     * path delivers lands in exactly that word. */
    {
        PsbcShaderMetadata base_vertex = *m, base_fragment = fragment;
        const uint32_t tables_zero[4] = {0u, 0u, 0u, 0u};
        /* Declared: the slot is reported and receives the delivered value. */
        *m = base_vertex; fragment = base_fragment;
        m->user_sgpr_count = 3; m->ngg_lds_layout_user_data_dword = 1;
        m->view_index_valid = true; m->view_index_user_data_dword = 2;
        assert(!ps5vk_runtime_draw_abi_build(m, &fragment, &abi));
        assert(abi.view_index_slot == 2);
        assert(!ps5vk_runtime_draw_values_sets(&abi, 31, 17, 0, 5, 0, 0,
            tables_zero, vertex, pixel));
        assert(vertex[2] == 5u);
        /* Absent: no slot is published, so the draw path has no word in which
         * to invent a value. */
        *m = base_vertex; fragment = base_fragment;
        m->user_sgpr_count = 3; m->ngg_lds_layout_user_data_dword = 1;
        assert(!ps5vk_runtime_draw_abi_build(m, &fragment, &abi));
        assert(abi.view_index_slot == UINT32_MAX);
        /* Both stages receive the same per-view value at independent slots. */
        *m = base_vertex; fragment = base_fragment;
        m->user_sgpr_count = 3; m->ngg_lds_layout_user_data_dword = 1;
        m->view_index_valid = true; m->view_index_user_data_dword = 2;
        fragment.user_sgpr_count = 2;
        fragment.view_index_valid = true; fragment.view_index_user_data_dword = 1;
        assert(!ps5vk_runtime_draw_abi_build(m, &fragment, &abi));
        assert(abi.fragment_view_index_valid && abi.fragment_view_index_slot==1);
        assert(!ps5vk_runtime_draw_values_sets(&abi,31,17,0,5,0,0,
            tables_zero,vertex,pixel));
        assert(vertex[2]==5 && pixel[1]==5);
        fragment.view_index_user_data_dword=fragment.user_sgpr_count;
        assert(ps5vk_runtime_draw_abi_build(m, &fragment, &abi));
        fragment.view_index_valid=false; fragment.view_index_user_data_dword=1;
        assert(ps5vk_runtime_draw_abi_build(m, &fragment, &abi));
        /* Slot collisions must reject before either bank is written. */
        uint32_t old_vertex[16],old_pixel[16];
        memcpy(old_vertex,vertex,sizeof(vertex));memcpy(old_pixel,pixel,sizeof(pixel));
        abi.push_constant_size=4; abi.fragment_push_slot=abi.fragment_view_index_slot;
        assert(ps5vk_runtime_draw_values_sets(&abi,31,17,0,5,0,4,
            tables_zero,vertex,pixel));
        assert(!memcmp(old_vertex,vertex,sizeof(vertex)) && !memcmp(old_pixel,pixel,sizeof(pixel)));
        abi.push_constant_size=0;abi.fragment_push_slot=UINT32_MAX;
        abi.fragment_descriptor_valid[0]=1;abi.fragment_descriptor_slot[0]=abi.fragment_view_index_slot;
        const uint32_t collision_tables[4]={16,0,0,0};
        assert(ps5vk_runtime_draw_values_sets(&abi,31,17,0,5,0,0,
            collision_tables,vertex,pixel));
        assert(!memcmp(old_vertex,vertex,sizeof(vertex)) && !memcmp(old_pixel,pixel,sizeof(pixel)));
        /* Malformed pairs are refused rather than discarded: invalid with a
         * nonzero dword, and valid but outside the declared block. */
        *m = base_vertex; fragment = base_fragment;
        m->view_index_valid = false; m->view_index_user_data_dword = 1;
        assert(ps5vk_runtime_draw_abi_build(m, &fragment, &abi));
        *m = base_vertex; fragment = base_fragment;
        m->view_index_valid = true;
        m->view_index_user_data_dword = m->user_sgpr_count;
        assert(ps5vk_runtime_draw_abi_build(m, &fragment, &abi));
        /* The draw path refuses the slot when it would collide with another
         * word of the same block or fall outside it. */
        struct ps5vk_runtime_draw_abi collided = {
            .enabled = 1, .vertex_count = 3, .fragment_count = 2,
            .base_vertex_slot = 0, .start_instance_slot = UINT32_MAX,
            .draw_id_slot = UINT32_MAX, .view_index_slot = 0,
            .lds_slot = 2, .lds_value = 0,
            .vertex_push_slot = UINT32_MAX, .fragment_push_slot = UINT32_MAX};
        assert(ps5vk_runtime_draw_values_sets(&collided, 1, 0, 0, 0, 0, 0,
            tables_zero, vertex, pixel));
        collided.view_index_slot = 2; /* collides with the LDS slot */
        assert(ps5vk_runtime_draw_values_sets(&collided, 1, 0, 0, 0, 0, 0,
            tables_zero, vertex, pixel));
        collided.view_index_slot = 3; /* outside the declared vertex block */
        assert(ps5vk_runtime_draw_values_sets(&collided, 1, 0, 0, 0, 0, 0,
            tables_zero, vertex, pixel));
        *m = base_vertex; fragment = base_fragment;
    }
    puts("Runtime shader header: pass (11 registers, relative semantics, rejection without mutation)");
    return 0;
}

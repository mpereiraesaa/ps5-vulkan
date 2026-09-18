/* Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * Compiler metadata adaptation. ABI references: local ps5_shader_header and
 * BlackBearReloaded's ps5-opengl ps5_agc_package.c (GPL-3.0-or-later).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "runtime_shader.h"
#include <string.h>

static void *relative(void *field, void *target)
{ return (void *)((uintptr_t)target - (uintptr_t)field); }

static int registers_valid(const PsbcRegisterWrite *r, uint32_t n, uint32_t max)
{
    if (!n || n > max) return 0;
    for (uint32_t i=0; i<n; ++i) {
        if (r[i].padding) return 0;
        for (uint32_t j=0; j<i; ++j) if (r[i].offset==r[j].offset) return 0;
    }
    return 1;
}

static const PsbcRegisterWrite *find(const PsbcRegisterWrite *r, uint32_t n, unsigned offset)
{
    for (uint32_t i=0;i<n;++i) if (r[i].offset==offset) return r+i;
    return NULL;
}

static unsigned count_bits(uint32_t value)
{
    unsigned bits=0;
    for (;value;value&=value-1) ++bits;
    return bits;
}

/* Clip and cull distances are exported through the packed position registers
 * past POS0: the pre-raster stage writes the combined components four at a time
 * into POS1/POS2, and the pixel state reads the enable bits back from
 * PA_CL_VS_OUT_CNTL. The compiled metadata carries both the exported-component
 * masks and the context registers that describe them, so this adapter requires
 * the two to agree instead of trusting a mask:
 *
 *  - the masks use the packed numbering the compiler exports (clip distances
 *    form the low components, cull distances continue immediately after them),
 *    and cannot exceed the eight components those two registers hold;
 *  - SPI_SHADER_POS_FORMAT must declare exactly POS0 plus one 4-component
 *    register per four exported components, and nothing else;
 *  - PA_CL_VS_OUT_CNTL must carry the clip mask in its clip enables, the whole
 *    packed mask in its cull enables, the two CCDIST vector enables that match
 *    them, and no miscellaneous vector a shader of this profile cannot write;
 *  - SPI_VS_OUT_CONFIG must count the parameter exports the compiled output
 *    semantics describe plus one parameter per CLIP_DIST0/1 or CULL_DIST0/1
 *    slot the masks occupy, which is how the standalone information pass
 *    accounts for distances a fragment stage never reads.
 *
 * The compiler's standalone information pass reserves those parameter slots
 * unconditionally (a linked pipeline only does so when the next stage reads the
 * distances), which is why a clip/cull shader reports PSBC_UNRESOLVED_AGC_LINKAGE:
 * the parameter semantics it could not resolve are exactly the distance slots
 * this function accounts for. The caller accepts that one unresolved field only
 * when this check proves the state agrees with the masks. */
static int distances_valid(const PsbcShaderMetadata *m,const PsbcRegisterWrite *cx,
                           uint32_t cx_count)
{
    const uint32_t clip=m->clip_distance_mask,cull=m->cull_distance_mask;
    const uint32_t total=clip|cull;
    if(!total)return 1;
    const unsigned clip_count=count_bits(clip),cull_count=count_bits(cull);
    const unsigned components=clip_count+cull_count;
    if(!components || components>8)return 0;
    if(clip!=((1u<<clip_count)-1u))return 0;
    if(cull!=(((1u<<cull_count)-1u)<<clip_count))return 0;
    const PsbcRegisterWrite *pos_format=find(cx,cx_count,0x1c3u);
    const PsbcRegisterWrite *vs_out=find(cx,cx_count,0x207u);
    const PsbcRegisterWrite *vs_out_config=find(cx,cx_count,0x1b1u);
    if(!pos_format || !vs_out || !vs_out_config)return 0;
    const unsigned pos_registers=1u+(components+3u)/4u;
    uint32_t expected_pos=0;
    for(unsigned i=0;i<pos_registers;++i)expected_pos|=4u<<(4u*i);
    if(pos_format->value!=expected_pos)return 0;
    uint32_t expected_out=clip|(total<<8);
    if(total&0x0fu)expected_out|=1u<<22;
    if(total&0xf0u)expected_out|=1u<<23;
    /* A second packed position register always travels on the miscellaneous
     * side bus on gfx10.3, and no other miscellaneous vector is representable
     * in this profile. */
    expected_out|=1u<<24;
    if(vs_out->value!=expected_out)return 0;
    /* Clip and cull distances share the two packed position registers, and each
     * register is an exported parameter the description now names: one word per
     * register with PSBC_SEMANTIC_DISTANCE_REGISTER + register in its low byte
     * and the register's parameter index above it. The description is required,
     * not derived: a stage that exported distances without naming the registers
     * would leave a pixel stage that reads one unmappable, and the parameter
     * count below can only be read off a complete description. */
    const unsigned slots=((total&0x0fu)?1u:0u)+((total&0xf0u)?1u:0u);
    for(unsigned r=0;r<slots;++r) {
        unsigned matches=0;
        for(uint32_t i=0;i<m->output_semantic_count;++i)
            matches+=(m->output_semantics[i]&255u)==(PSBC_SEMANTIC_DISTANCE_REGISTER+r);
        if(matches!=1)return 0;
    }
    const unsigned parameters=m->output_semantic_count;
    const uint32_t expected_config=(uint32_t)(((parameters?parameters:1u)-1u)<<1);
    return vs_out_config->value==expected_config;
}

static ps5_agc_register convert(PsbcRegisterWrite r)
{ return (ps5_agc_register){r.offset,r.value}; }

static int descriptors_valid(const PsbcShaderMetadata *m)
{
    if(m->descriptor_binding_count>PSBC_MAX_DESCRIPTOR_BINDINGS ||
       m->descriptor_set0_valid!=m->descriptor_set_valid[0] ||
       m->descriptor_set0_user_data_dword!=m->descriptor_set_user_data_dword[0])return 0;
    uint32_t declared=0,counts[PSBC_MAX_DESCRIPTOR_SETS]={0};
    for(unsigned i=0;i<m->descriptor_binding_count;++i) {
        const PsbcDescriptorBinding *b=&m->descriptor_bindings[i];
        uint32_t stride=0;
        switch(b->type) {
        case PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER:stride=48;break;
        /* The resource-only image record an input attachment is read through:
         * the same eight DWORDs a sampled T# occupies, with no sampler words,
         * so a metadata record that claims 48 bytes for this type - or 32 for
         * a combined pair - is refused instead of being sized by its words. */
        case PSBC_DESCRIPTOR_INPUT_ATTACHMENT:stride=32;break;
        case PSBC_DESCRIPTOR_UNIFORM_BUFFER:
        case PSBC_DESCRIPTOR_STORAGE_BUFFER:
        case PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER:stride=16;break;
        default:return 0;
        }
        if(b->set>=PSBC_MAX_DESCRIPTOR_SETS || b->binding>=32 || !b->array_size ||
           b->array_size>128 || b->stride!=stride || b->offset%16 ||
           b->offset>6144 || b->array_size>(6144-b->offset)/stride)return 0;
        counts[b->set]+=b->array_size;if(counts[b->set]>128)return 0;
        uint32_t end=b->offset+b->array_size*stride;
        for(unsigned j=0;j<i;++j) {
            const PsbcDescriptorBinding *a=&m->descriptor_bindings[j];
            if(a->set!=b->set)continue;
            if(a->binding==b->binding ||
               (b->offset<a->offset+a->array_size*a->stride && a->offset<end))return 0;
        }
        declared|=1u<<b->set;
    }
    uint32_t used=0;
    for(unsigned s=0;s<PSBC_MAX_DESCRIPTOR_SETS;++s) {
        uint32_t slot=m->descriptor_set_user_data_dword[s];
        if(m->descriptor_set_valid[s]) {
            if(!(declared&(1u<<s)) || slot>=m->user_sgpr_count || slot>=16 ||
               (used&(1u<<slot)))return 0;
            used|=1u<<slot;
        } else if(slot)return 0;
    }
    return 1;
}

/* A compiler-declared user-SGPR slot is either absent, and then the dword it
 * names must be zero - a nonzero value for an absent slot is a malformed pair
 * that must not be silently discarded - or present and inside the block the
 * stage declared, which is what the draw-parameter slot checks enforce on top. */
static int slot_pair_ok(bool valid,uint32_t dword,uint32_t user_sgpr_count)
{
    return valid ? dword<user_sgpr_count : dword==0;
}

int ps5vk_runtime_draw_abi_build(const PsbcShaderMetadata *v,
    const PsbcShaderMetadata *f,struct ps5vk_runtime_draw_abi *out)
{
    if(!v || !f || !out || v->version!=PSBC_SHADER_METADATA_VERSION || f->version!=PSBC_SHADER_METADATA_VERSION ||
       v->vertex_buffer_per_attribute || f->vertex_buffer_usage_mask || f->vertex_buffer_per_attribute ||
       /* Draw parameters are vertex-only; ViewIndex has independently
        * declared vertex and fragment slots. Absent pairs must be zero. */
       f->draw_id_valid ||
       f->base_vertex_valid || f->start_instance_valid ||
       !slot_pair_ok(v->base_vertex_valid,v->base_vertex_user_data_dword,v->user_sgpr_count) ||
       !slot_pair_ok(v->start_instance_valid,v->start_instance_user_data_dword,v->user_sgpr_count) ||
       !slot_pair_ok(v->draw_id_valid,v->draw_id_user_data_dword,v->user_sgpr_count) ||
       !slot_pair_ok(v->view_index_valid,v->view_index_user_data_dword,v->user_sgpr_count) ||
       !slot_pair_ok(f->base_vertex_valid,f->base_vertex_user_data_dword,f->user_sgpr_count) ||
       !slot_pair_ok(f->start_instance_valid,f->start_instance_user_data_dword,f->user_sgpr_count) ||
       !slot_pair_ok(f->draw_id_valid,f->draw_id_user_data_dword,f->user_sgpr_count) ||
       !slot_pair_ok(f->view_index_valid,f->view_index_user_data_dword,f->user_sgpr_count) ||
       /* The published window must hold the whole block, and the two system
        * registers a merged pair gates on must lie below it: the driver cannot
        * address the system block, so a pair that reports them inside the
        * window is a contract violation, not something to write. */
       v->user_data_window_base>16 ||
       (v->user_data_window_base && v->user_data_window_base+v->user_sgpr_count>16) ||
       (v->esgs_system_sgprs_valid &&
        (!v->user_data_window_base ||
         v->esgs_gs_tg_info_sgpr>=v->user_data_window_base ||
         v->esgs_merged_wave_info_sgpr>=v->user_data_window_base ||
         v->esgs_gs_tg_info_sgpr==v->esgs_merged_wave_info_sgpr)) ||
       (v->source_stage!=PSBC_STAGE_VERTEX && v->source_stage!=PSBC_STAGE_GEOMETRY &&
        v->source_stage!=PSBC_STAGE_TESS_EVAL) ||
       v->hardware_stage!=PSBC_HW_STAGE_NGG || f->source_stage!=PSBC_STAGE_FRAGMENT ||
       f->hardware_stage!=PSBC_HW_STAGE_PIXEL || !v->ngg_lds_layout_valid ||
       v->output_semantic_count>PSBC_MAX_SEMANTICS || f->input_semantic_count>PSBC_MAX_SEMANTICS ||
       !descriptors_valid(v) || !descriptors_valid(f))
        return -1;
    for(uint32_t i=0;i<f->input_semantic_count;++i) {
        unsigned matches=0;
        for(uint32_t j=0;j<v->output_semantic_count;++j)
            matches+=(f->input_semantics[i]&255u)==(v->output_semantics[j]&255u);
        if(matches!=1)return -1;
    }
    struct ps5vk_runtime_draw_abi abi={.enabled=1,.vertex_count=v->user_sgpr_count,
        .fragment_count=f->user_sgpr_count,
        .base_vertex_slot=v->base_vertex_valid?v->base_vertex_user_data_dword:UINT32_MAX,
        .start_instance_slot=v->start_instance_valid?v->start_instance_user_data_dword:UINT32_MAX,
        .draw_id_slot=v->draw_id_valid?v->draw_id_user_data_dword:UINT32_MAX,
        .view_index_slot=v->view_index_valid?v->view_index_user_data_dword:UINT32_MAX,
        .fragment_view_index_valid=f->view_index_valid,
        .fragment_view_index_slot=f->view_index_user_data_dword,
        .vertex_buffer_valid=v->vertex_buffer_table_valid,
        .vertex_buffer_slot=v->vertex_buffer_table_user_data_dword,
        .vertex_buffer_usage_mask=v->vertex_buffer_usage_mask,
        .lds_slot=v->ngg_lds_layout_user_data_dword,.lds_value=v->ngg_lds_layout,
        /* The pre-raster stage's ring descriptor table, when it declares one.
         * Only a tessellation DOMAIN does: the PS5 argument path declares the
         * table as a user SGPR pair for both tessellation stages because the
         * system-block ring_offsets is not writable here. The address is not
         * known yet - the pipeline allocates the rings after this - so the
         * slot is recorded now and the owner fills the two words in once the
         * block exists. A stage that declares no table leaves all four fields
         * zero, which the value builder rejects if any of them is set. */
        .ring_table_valid=v->ps5_ring_table_valid,
        .ring_table_slot=v->ps5_ring_table_valid?
            v->ps5_ring_table_user_data_dword:0u,
        /* Where the compiler says the driver's block starts, and the two system
         * registers a merged pair gates on. They are recorded, not written: the
         * base was measured below the window on every compiled program, so the
         * driver has no way to address them, and a pair that reports them inside
         * the window is refused below instead of being written where the shader
         * will never look. */
        .window_base=v->user_data_window_base,
        .esgs_described=v->esgs_system_sgprs_valid,
        .esgs_gs_tg_info_sgpr=v->esgs_gs_tg_info_sgpr,
        .esgs_merged_wave_info_sgpr=v->esgs_merged_wave_info_sgpr,
        .vertex_push_slot=v->push_constants_valid?v->push_constants_user_data_dword:UINT32_MAX,

        .fragment_push_slot=f->push_constants_valid?f->push_constants_user_data_dword:UINT32_MAX,
        .push_constant_size=v->push_constant_size>f->push_constant_size?
            v->push_constant_size:f->push_constant_size};
    uint32_t tables[PS5VK_RUNTIME_DESCRIPTOR_SETS]={0};
    _Static_assert(PS5VK_RUNTIME_DESCRIPTOR_SETS==PSBC_MAX_DESCRIPTOR_SETS,"descriptor set ABI");
    for(unsigned s=0;s<PS5VK_RUNTIME_DESCRIPTOR_SETS;++s) {
        abi.vertex_descriptor_valid[s]=v->descriptor_set_valid[s];
        abi.fragment_descriptor_valid[s]=f->descriptor_set_valid[s];
        abi.vertex_descriptor_slot[s]=v->descriptor_set_user_data_dword[s];
        abi.fragment_descriptor_slot[s]=f->descriptor_set_user_data_dword[s];
        abi.vertex_used_bindings[s]=v->descriptor_used_binding_mask[s];
        abi.fragment_used_bindings[s]=f->descriptor_used_binding_mask[s];
        if(abi.vertex_descriptor_valid[s] || abi.fragment_descriptor_valid[s])tables[s]=16*(s+1);
    }
    uint32_t vertex[16],pixel[16];
    /* Validate slot bounds/collisions before publishing either register bank. */
    if(ps5vk_runtime_draw_values_sets(&abi,0,0,0,0,abi.vertex_buffer_valid?16:0,
        abi.push_constant_size?4:0,tables,
        vertex,pixel))return -1;
    *out=abi;
    return 0;
}

int ps5vk_runtime_shader_build(struct ps5vk_runtime_shader *d, const PsbcShaderOutput *c)
{
    if (!d || !c || !c->machine_code || !c->machine_code_size ||
        (c->machine_code_size & 3u) || c->machine_code_size>16u*1024u*1024u) return -1;
    const PsbcShaderMetadata *m=&c->metadata;
    /* The pre-raster stage is the vertex program, the merged vertex+geometry
     * program, or the tessellation domain half: all three run on the NGG
     * hardware stage and export vertices, so their source stages share the
     * pre-raster shape. */
    const int has_geometry=m->source_stage==PSBC_STAGE_GEOMETRY;
    const int has_domain=m->source_stage==PSBC_STAGE_TESS_EVAL;
    int vs=(m->source_stage==PSBC_STAGE_VERTEX || has_geometry || has_domain) &&
        m->hardware_stage==PSBC_HW_STAGE_NGG;
    int fs=m->source_stage==PSBC_STAGE_FRAGMENT && m->hardware_stage==PSBC_HW_STAGE_PIXEL;
    if ((!vs && !fs) || m->version!=PSBC_SHADER_METADATA_VERSION || m->target!=PSBC_TARGET_PS5 ||
        m->address32_hi!=2 || m->user_sgpr_count>16 || m->scratch_valid ||
        m->scratch_bytes_per_wave || m->scratch_size_per_thread || m->streamout_valid ||
        m->input_semantic_count>PSBC_MAX_SEMANTICS || m->output_semantic_count>PSBC_MAX_SEMANTICS ||
        (vs && m->input_semantic_count) || (fs && m->output_semantic_count) ||
        (m->unresolved_fields & ~(PSBC_UNRESOLVED_PROGRAM_CHECKSUM |
            (vs ? (PSBC_UNRESOLVED_NGG_ESGS_RING_ITEMSIZE |
                   ((m->clip_distance_mask || m->cull_distance_mask) ?
                    PSBC_UNRESOLVED_AGC_LINKAGE : 0)) : 0)))) return -2;
    /* Distances are a pre-raster export only, and their masks must agree with
     * the state the same compiler emitted for them. */
    if(vs) {
        if(!distances_valid(m,m->context_registers,m->context_register_count))return -2;
    } else if(m->clip_distance_mask || m->cull_distance_mask) return -2;
    if (m->vertex_buffer_table_valid ?
        (!vs || m->vertex_buffer_table_user_data_dword>=m->user_sgpr_count ||
         !m->vertex_buffer_usage_mask || m->vertex_buffer_usage_mask>0xffffu || m->vertex_buffer_per_attribute) :
        (m->vertex_buffer_table_user_data_dword || m->vertex_buffer_usage_mask || m->vertex_buffer_per_attribute)) return -2;
    if(!descriptors_valid(m))return -2;
    if (m->push_constants_valid ?
        (!m->push_constant_size || m->push_constant_size>256 ||
         m->push_constants_user_data_dword>=m->user_sgpr_count) :
        (m->push_constant_size || m->push_constants_user_data_dword)) return -2;
    if (!slot_pair_ok(m->base_vertex_valid,m->base_vertex_user_data_dword,m->user_sgpr_count) ||
        !slot_pair_ok(m->start_instance_valid,m->start_instance_user_data_dword,m->user_sgpr_count) ||
        !slot_pair_ok(m->draw_id_valid,m->draw_id_user_data_dword,m->user_sgpr_count) ||
        !slot_pair_ok(m->view_index_valid,m->view_index_user_data_dword,m->user_sgpr_count) ||
        /* ViewIndex is also a fragment input, unlike these draw parameters. */
        (!vs && (m->base_vertex_valid || m->start_instance_valid || m->draw_id_valid))) return -2;
    if (!registers_valid(m->context_registers,m->context_register_count,PSBC_MAX_CONTEXT_REGISTERS) ||
        !registers_valid(m->shader_registers,m->shader_register_count,PSBC_MAX_SHADER_REGISTERS)) return -2;
    unsigned lo=vs?0xc8:8, rsrc=vs?0x8a:0xa;
    const PsbcRegisterWrite *low=find(m->shader_registers,m->shader_register_count,lo);
    const PsbcRegisterWrite *high=find(m->shader_registers,m->shader_register_count,lo+1);
    if (!low || !high || low->value || high->value ||
        !find(m->shader_registers,m->shader_register_count,rsrc) ||
        !find(m->shader_registers,m->shader_register_count,rsrc+1)) return -3;
    if (vs && (!m->linkage_valid || m->linkage_ge_cntl.offset!=0x25b ||
        m->linkage_stages_en.offset!=0x2d5 || m->linkage_user_vgpr_en.offset!=0x262 ||
        !find(m->context_registers,m->context_register_count,0x2ab) ||
        !m->ngg_lds_layout_valid || m->ngg_lds_layout_user_data_dword>=m->user_sgpr_count ||
        m->ngg_lds_layout>UINT16_MAX)) return -3;
    /* A geometry stage's pipeline state is the merged program's: the output
     * topology, the maximum vertices it may emit, the subgroup and on-chip
     * limits and the ring item size all have to be present, or the GE would run
     * with whatever the previous pipeline left behind. */
    if (has_geometry) {
        static const unsigned geometry_registers[]={0x1ffu,0x291u,0x29bu,0x2abu,0x2ceu,0x2d3u};
        for (unsigned i=0;i<sizeof(geometry_registers)/sizeof(geometry_registers[0]);++i)
            if(!find(m->context_registers,m->context_register_count,geometry_registers[i]))
                return -3;
    }
    /* The tessellation domain half's pipeline state: the subgroup/on-chip
     * limits, the ring item size, the instance count and the maximum vertices
     * one invocation emits (one for a passthrough domain) have to be present,
     * or the GE would run with whatever the previous pipeline left behind. The
     * output primitive type is the one register this list does NOT need: a
     * tessellated patch's shape comes from VGT_TF_PARAM, which the hull half
     * publishes, not from the domain. */
    if (has_domain) {
        static const unsigned domain_registers[]={0x1ffu,0x291u,0x2abu,0x2ceu,0x2d3u};
        for (unsigned i=0;i<sizeof(domain_registers)/sizeof(domain_registers[0]);++i)
            if(!find(m->context_registers,m->context_register_count,domain_registers[i]))
                return -3;
    }
    if (fs && (m->linkage_valid || m->ngg_lds_layout_valid)) return -3;
    if(fs) {
        const PsbcRegisterWrite *z=find(m->context_registers,m->context_register_count,0x1c4);
        const PsbcRegisterWrite *mask=find(m->context_registers,m->context_register_count,0x8f);
        if(!z || z->value || !mask || mask->value!=15)return -3;
    }
    memset(d,0,sizeof(*d));
    d->header.file_header=0x34333231; d->header.version=24;
    d->header.header_size=sizeof(*d); d->header.shader_size=(uint32_t)c->machine_code_size;
    d->header.target=5; d->header.type=vs?PS5_SHADER_PRE_RASTER:PS5_SHADER_PIXEL;
    d->header.num_cx_registers=(uint8_t)m->context_register_count;
    d->header.num_sh_registers=(uint8_t)m->shader_register_count;
    d->header.user_data=relative(&d->header.user_data,&d->resources);
    d->header.cx_registers=relative(&d->header.cx_registers,d->context);
    d->header.sh_registers=relative(&d->header.sh_registers,d->shader);
    for (uint32_t i=0;i<m->context_register_count;++i) {
        d->context[i]=convert(m->context_registers[i]);
        /* VGT_ESGS_RING_ITEMSIZE (0x2ab) is the hardware's ES->GS item size, and
         * the hardware scales the per-vertex offsets it hands the merged
         * geometry half by it. Next-gen geometry keeps it at ONE so that those
         * offsets are ITEM INDICES: upstream never writes the register on the
         * NGG path (radv's register precompute for next-gen geometry does not
         * touch it and the state initialiser sets 1) and lets the shader carry
         * the item size instead - which is what this compiler's addressing does
         * too, multiplying the offset by the item size in dwords and then by
         * four. Programming the compiler's legacy value here scales the offsets
         * twice, and that is measured, not inferred: with the compiler's 5
         * programmed, the geometry half read item 5k where it must read item k
         * (k = 3p+index), so only the first vertex of each primitive - the one
         * whose offset is zero whatever the scale - ever came back correct, and
         * the second and third read another primitive's vertex. A vertex-only
         * program wants the same 1 for the same reason. */
        if (vs && d->context[i].offset==0x2ab) d->context[i].value=1;
    }
    for (uint32_t i=0;i<m->shader_register_count;++i) d->shader[i]=convert(m->shader_registers[i]);
    if (vs) {
        d->header.specials=relative(&d->header.specials,&d->specials);
        d->header.special_sizes_bytes=sizeof(d->specials);
        d->specials.ge_cntl=convert(m->linkage_ge_cntl);
        d->specials.shader_stages_en=convert(m->linkage_stages_en);
        d->specials.ge_user_vgpr_en=convert(m->linkage_user_vgpr_en);
    }
    d->header.num_input_semantics=m->input_semantic_count;
    d->header.num_output_semantics=(uint16_t)m->output_semantic_count;
    if (m->input_semantic_count) {
        memcpy(d->inputs,m->input_semantics,m->input_semantic_count*4u);
        d->header.input_semantics=relative(&d->header.input_semantics,d->inputs);
    }
    if (m->output_semantic_count) {
        memcpy(d->outputs,m->output_semantics,m->output_semantic_count*4u);
        d->header.output_semantics=relative(&d->header.output_semantics,d->outputs);
    }
    return 0;
}

/* The hull half's loader view: the LS and HS programs, prepared without the
 * AGC linker (which has no hull support) and without the tessellation launch
 * state, which the native create path owns and programs: the stage enables
 * come from the domain half's linked state with the LS/HS enables ORed in,
 * the LS_HS_CONFIG from the workgroup layout this compile published, and the
 * program addresses are patched here from the code the create path loads.
 * The tessellation-pipeline unresolved bit is exactly why this builder
 * exists, so it is checked as present, not refused. The pgm lo/hi values stay
 * zero through this call, which is what the address patch keys on.
 * Returns 0 on success. */
int ps5vk_runtime_hull_build(struct ps5vk_runtime_shader *hull,
    const PsbcShaderOutput *c)
{
    if (!hull || !c || !c->machine_code || !c->machine_code_size ||
        (c->machine_code_size & 3u) || c->machine_code_size>16u*1024u*1024u) return -1;
    const PsbcShaderMetadata *m=&c->metadata;
    /* The merged pair is a launchable hull program, so it must NOT carry the
     * tessellation-pipeline unresolved bit any more: a control half compiled
     * without its vertex half still does, and is still refused here. */
    if (m->version!=PSBC_SHADER_METADATA_VERSION || m->target!=PSBC_TARGET_PS5 ||
        m->source_stage!=PSBC_STAGE_TESS_CTRL ||
        m->hardware_stage!=PSBC_HW_STAGE_HULL ||
        !m->hull_tess_wg_valid ||
        !m->hull_num_patches_per_wg || m->hull_num_patches_per_wg>255 ||
        !m->hull_tcs_lds_size ||
        /* The ring descriptor table must have a user-data home: the hull
         * dereferences the table on every patch draw and cannot be launched
         * without it on this platform. */
        !m->ps5_ring_table_valid ||
        m->ps5_ring_table_user_data_dword+1u>=m->user_sgpr_count ||
        /* One image: the separate LS carriage the two-program model used is
         * gone, and a package still claiming it is not this ABI. */
        m->hull_ls_valid || m->hull_ls_code_offset || m->hull_ls_code_size ||
        m->user_sgpr_count>16 || m->scratch_valid || m->scratch_bytes_per_wave ||
        /* The linked stage enables are the DOMAIN half's publication (the
         * domain's NGG linkage carries ES_EN/PRIMGEN and the driver ORs the
         * LS/HS enables in), so the hull does not carry a linkage block. */
        m->linkage_valid ||
        (m->unresolved_fields & ~PSBC_UNRESOLVED_PROGRAM_CHECKSUM)) return -2;
    /* On GFX10 the merged stage is ONE program counter and the pinned
     * radv_get_shader_regs() puts it at the LS block (R_00B520/R_00B524, sh
     * 0x148/0x149) while the resource pair stays at the HS block
     * (R_00B428/R_00B42C, sh 0x10a/0x10b). R_00B420 PGM_LO_HS is the address
     * register only below GFX9 and must not appear. */
    if (m->shader_register_count!=4) return -3;
    const PsbcRegisterWrite *lo=find(m->shader_registers,m->shader_register_count,0x148);
    const PsbcRegisterWrite *hi=find(m->shader_registers,m->shader_register_count,0x149);
    const PsbcRegisterWrite *rsrc1=find(m->shader_registers,m->shader_register_count,0x10a);
    const PsbcRegisterWrite *rsrc2=find(m->shader_registers,m->shader_register_count,0x10b);
    if (!lo || !hi || !rsrc1 || !rsrc2 || lo->value || hi->value ||
        !rsrc1->value || !rsrc2->value) return -3;
    if (find(m->shader_registers,m->shader_register_count,0x108) ||
        find(m->shader_registers,m->shader_register_count,0x109)) return -3;
    /* The tessellator's own configuration is the one context register this
     * stage fully determines, and the launch state needs it from here. */
    if (!find(m->context_registers,m->context_register_count,0x2db)) return -3;
    if (!registers_valid(m->context_registers,m->context_register_count,PSBC_MAX_CONTEXT_REGISTERS) ||
        !descriptors_valid(m)) return -4;
    if (m->descriptor_set_valid[0] || m->descriptor_set_valid[1] ||
        m->descriptor_set_valid[2] || m->descriptor_set_valid[3] ||
        m->push_constants_valid || m->push_constant_size ||
        m->vertex_buffer_table_valid) return -5;
    memset(hull,0,sizeof(*hull));
    hull->header.file_header=0x34333231; hull->header.version=24;
    hull->header.header_size=sizeof(*hull);
    hull->header.shader_size=(uint32_t)c->machine_code_size;
    hull->header.target=5; hull->header.type=PS5_SHADER_PRE_RASTER;
    hull->header.num_cx_registers=(uint8_t)m->context_register_count;
    hull->header.cx_registers=relative(&hull->header.cx_registers,hull->context);
    hull->header.num_sh_registers=4;
    hull->header.sh_registers=relative(&hull->header.sh_registers,hull->shader);
    /* The header AGC itself would accept, not just the register container this
     * driver reads. Every authorised AGC header carries a user-data block and
     * a specials block; a hand-built header that leaves those pointers null is
     * structurally unlike anything the native constructor has seen, and it
     * crashes inside sceAgcCreateShader rather than returning an error.
     * The blocks are present and zeroed here: the hull has no linkage block to
     * publish (its metadata deliberately carries linkage_valid = false, the
     * stage enables being the domain's), so there is nothing to put in the
     * specials beyond making them exist. */
    hull->header.user_data=relative(&hull->header.user_data,&hull->resources);
    hull->header.specials=relative(&hull->header.specials,&hull->specials);
    hull->header.special_sizes_bytes=sizeof(hull->specials);
    for (uint32_t i=0;i<m->context_register_count;++i) hull->context[i]=convert(m->context_registers[i]);
    for (uint32_t i=0;i<4;++i) hull->shader[i]=convert(m->shader_registers[i]);
    return 0;
}

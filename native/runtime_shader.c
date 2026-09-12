/* Compiler metadata adaptation. ABI references: local ps5_shader_header and
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

static ps5_agc_register convert(PsbcRegisterWrite r)
{ return (ps5_agc_register){r.offset,r.value}; }

int ps5vk_runtime_draw_abi_build(const PsbcShaderMetadata *v,
    const PsbcShaderMetadata *f,struct ps5vk_runtime_draw_abi *out)
{
    if(!v || !f || !out || v->source_stage!=PSBC_STAGE_VERTEX ||
       v->hardware_stage!=PSBC_HW_STAGE_NGG || f->source_stage!=PSBC_STAGE_FRAGMENT ||
       f->hardware_stage!=PSBC_HW_STAGE_PIXEL || !v->ngg_lds_layout_valid ||
       v->output_semantic_count>PSBC_MAX_SEMANTICS || f->input_semantic_count>PSBC_MAX_SEMANTICS)
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
        .lds_slot=v->ngg_lds_layout_user_data_dword,.lds_value=v->ngg_lds_layout};
    uint32_t vertex[16],pixel[16];
    if(ps5vk_runtime_draw_values(&abi,0,0,vertex,pixel))return -1;
    *out=abi;
    return 0;
}

int ps5vk_runtime_shader_build(struct ps5vk_runtime_shader *d, const PsbcShaderOutput *c)
{
    if (!d || !c || !c->machine_code || !c->machine_code_size ||
        (c->machine_code_size & 3u) || c->machine_code_size>16u*1024u*1024u) return -1;
    const PsbcShaderMetadata *m=&c->metadata;
    int vs=m->source_stage==PSBC_STAGE_VERTEX && m->hardware_stage==PSBC_HW_STAGE_NGG;
    int fs=m->source_stage==PSBC_STAGE_FRAGMENT && m->hardware_stage==PSBC_HW_STAGE_PIXEL;
    if ((!vs && !fs) || m->version!=PSBC_SHADER_METADATA_VERSION || m->target!=PSBC_TARGET_PS5 ||
        m->address32_hi!=2 || m->user_sgpr_count>16 || m->scratch_valid ||
        m->scratch_bytes_per_wave || m->scratch_size_per_thread || m->streamout_valid ||
        m->descriptor_binding_count || m->descriptor_set0_valid || m->vertex_buffer_table_valid ||
        m->clip_distance_mask || m->cull_distance_mask ||
        m->input_semantic_count>PSBC_MAX_SEMANTICS || m->output_semantic_count>PSBC_MAX_SEMANTICS ||
        (vs && m->input_semantic_count) || (fs && m->output_semantic_count) ||
        (m->unresolved_fields & ~(PSBC_UNRESOLVED_PROGRAM_CHECKSUM |
            (vs ? PSBC_UNRESOLVED_NGG_ESGS_RING_ITEMSIZE : 0)))) return -2;
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
        /* NGG VS exports unscaled vertex indices; no API geometry shader. */
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

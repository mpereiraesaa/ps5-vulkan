/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Compiler contract for the tessellation pair at the pinned dependency.
 *
 * This is what the pinned compiler (opengnm-psbc cce48c6, metadata version 17)
 * really produces for the pinned fixtures, asserted so a dependency move that
 * changes the contract fails here instead of inside the driver. Measured, not
 * assumed:
 *
 *   - psbc_compile_tess_pipeline() compiles the vertex and control halves
 *     TOGETHER, in one ACO call, and publishes ONE merged LS/HS image. That
 *     is what makes the vertex half a real LS: RADV's gather_shader_info_vs()
 *     sets vs.as_ls only when next_stage is MESA_SHADER_TESS_CTRL, so a half
 *     compiled standalone came out as a legacy VS that exported to the
 *     parameter cache instead of writing its outputs to LDS for the control
 *     half - and two such images concatenated have no merged entry for the
 *     single program counter the hardware launches LS/HS from.
 *   - On GFX10 that one program counter is the LS block (R_00B520/R_00B524)
 *     with the resource pair at the HS block (R_00B428/R_00B42C), per the
 *     pinned radv_get_shader_regs(); R_00B420 PGM_LO_HS is the address
 *     register only below GFX9 and is not published here.
 *   - The package is launchable, so it no longer carries
 *     PSBC_UNRESOLVED_TESS_PIPELINE. A control half compiled ALONE still does,
 *     and the hull loader still refuses it.
 *   - LDS_SIZE in RSRC2_HS stays zero from the compiler: it depends on the
 *     patch count, a pipeline property, so the driver owns it. The compiler
 *     publishes the byte count through hull_tcs_lds_size instead.
 *   - The DOMAIN half's loadable package is the NGG form, with the offchip
 *     system SGPRs the tessellation args declare.
 */
#include "libpsbc/psbc_compile.h"
#include "runtime_shader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *read_module(const char *path, size_t *word_count)
{
    FILE *f=fopen(path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>0 && bytes%4==0);
    rewind(f);uint32_t *words=malloc((size_t)bytes);assert(words);
    assert(fread(words,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    *word_count=(size_t)bytes/4;return words;
}

static const PsbcRegisterWrite *find_register(const PsbcShaderMetadata *m,
    const PsbcRegisterWrite *registers,uint32_t count,uint16_t offset)
{
    for(uint32_t i=0;i<count;++i)
        if(registers[i].offset==offset)return &registers[i];
    (void)m;return NULL;
}

int main(void)
{
    size_t vn,hn,en;
    uint32_t *vs=read_module("build/runtime-graphics/tess.vert.spv",&vn);
    uint32_t *hs=read_module("build/runtime-graphics/tess.tesc.spv",&hn);
    uint32_t *es=read_module("build/runtime-graphics/tess.tese.spv",&en);

    /* The reference LS config: the vertex half compiled the way the tess
     * pipeline compiles it, so the published hull LS registers can be checked
     * against an independent compilation of the same module. The standalone
     * compile keeps the unshifted VS block (SPI_SHADER_PGM 0x48..), while the
     * pipeline republishes the same config at the LS block (0x148..), so only
     * the VALUES are the cross-check. */
    PsbcCompileOptions reference={
        .target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_VERTEX,.entrypoint="main",
        .optimise=true,.address32_hi=2,.primitive_type=4,
        .rasterization_samples=1};
    PsbcShaderOutput standalone_vs={0};
    assert(psbc_compile_shader(vs,vn*4,&reference,&standalone_vs)==PSBC_RESULT_OK);
    const PsbcRegisterWrite *vs_rsrc1=find_register(NULL,
        standalone_vs.metadata.shader_registers,
        standalone_vs.metadata.shader_register_count,0x4a);
    const PsbcRegisterWrite *vs_rsrc2=find_register(NULL,
        standalone_vs.metadata.shader_registers,
        standalone_vs.metadata.shader_register_count,0x4b);
    assert(vs_rsrc1 && vs_rsrc2);

    /* The pipeline's INPUT patch size. The merged pair needs it: the LSHS
     * workgroup layout is derived from it, and without it the compile is
     * refused rather than publishing an unlaunchable package. */
    PsbcCompileOptions options={
        .target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_TESS_CTRL,.entrypoint="main",
        .optimise=true,.address32_hi=2,.primitive_type=4,
        .rasterization_samples=1,.patch_control_points=3};
    PsbcShaderOutput hull={0};
    assert(psbc_compile_tess_pipeline(vs,vn*4,hs,hn*4,es,en*4,&options,&hull)==
        PSBC_RESULT_OK);
    const PsbcShaderMetadata *m=&hull.metadata;
    assert(m->version==PSBC_SHADER_METADATA_VERSION);
    assert(m->target==PSBC_TARGET_PS5);
    assert(m->source_stage==PSBC_STAGE_TESS_CTRL);
    /* ONE merged LS/HS program. The halves are compiled together by ACO, so
     * the vertex half is a real LS - RADV's gather_shader_info_vs() sets
     * vs.as_ls only when next_stage is TESS_CTRL - and not a legacy VS that
     * exports to the parameter cache. */
    assert(m->hardware_stage==PSBC_HW_STAGE_HULL);
    /* A launchable hull program: the tessellation-pipeline bit is gone and
     * only the checksum remains unresolved. */
    assert(!(m->unresolved_fields & PSBC_UNRESOLVED_TESS_PIPELINE));
    assert(!(m->unresolved_fields & ~PSBC_UNRESOLVED_PROGRAM_CHECKSUM));
    /* One image: the separate LS carriage the two-program model used is gone. */
    assert(!m->hull_ls_valid);
    assert(!m->hull_ls_code_offset && !m->hull_ls_code_size);
    assert(hull.machine_code_size%4==0);
    /* The same control shader compiled ALONE still reports the old
     * unlaunchable shape. */
    PsbcShaderOutput solo={0};
    assert(psbc_compile_shader(hs,hn*4,&options,&solo)==PSBC_RESULT_OK);
    assert(solo.metadata.hardware_stage==PSBC_HW_STAGE_UNKNOWN);
    assert(solo.metadata.unresolved_fields & PSBC_UNRESOLVED_TESS_PIPELINE);
    /* And it produces MORE code than the linked pair, which is the opposite
     * of what this test asserted before the evaluation half was linked in.
     *
     * The old assertion used total size as a proxy for "the merged image
     * really contains the vertex half". That proxy is now wrong, and it is
     * wrong for the right reason: an unlinked control half cannot know the
     * tessellator's domain - the .tese declares it - so
     * ac_nir_lower_tess_io_to_mem emits a RUNTIME three-way branch on
     * nir_load_tcs_primitive_mode_amd with a separate factor store for
     * triangles, isolines and quads, plus a conditional store of the levels
     * for a TES that might read them. Linking the evaluation half resolves
     * both to compile-time constants and the dead arms disappear, which is
     * worth more than the vertex half's few instructions cost.
     *
     * Asserting the direction pins that: a dependency move that stopped
     * resolving the primitive mode would put the branches back and fail
     * here, instead of silently restoring a hull that stores quad-shaped
     * tessellation factors for a triangle domain. */
    assert(hull.machine_code_size<solo.machine_code_size);
    psbc_free_output(&solo);
    /* On GFX10 the merged stage is ONE program counter, and the pinned
     * radv_get_shader_regs() puts it at the LS block with the resource pair at
     * the HS block. R_00B420 PGM_LO_HS is the address register only below
     * GFX9, so it must NOT be published for this target. */
    const PsbcRegisterWrite *pgm_lo=find_register(m,
        m->shader_registers,m->shader_register_count,0x148);
    const PsbcRegisterWrite *pgm_hi=find_register(m,
        m->shader_registers,m->shader_register_count,0x149);
    assert(pgm_lo && pgm_hi && !pgm_lo->value && !pgm_hi->value);
    assert(!find_register(m,m->shader_registers,m->shader_register_count,0x108));
    assert(!find_register(m,m->shader_registers,m->shader_register_count,0x109));
    const PsbcRegisterWrite *hs_rsrc1=find_register(m,
        m->shader_registers,m->shader_register_count,0x10a);
    const PsbcRegisterWrite *hs_rsrc2=find_register(m,
        m->shader_registers,m->shader_register_count,0x10b);
    assert(hs_rsrc1 && hs_rsrc2 && hs_rsrc1->value && hs_rsrc2->value);
    assert(m->shader_register_count==4);
    /* The merged config is genuinely merged: it is NOT the standalone vertex
     * half's config, which is what the old two-program model republished. */
    assert(hs_rsrc1->value!=vs_rsrc1->value ||
           hs_rsrc2->value!=vs_rsrc2->value);
    /* LDS_SIZE (bits 18..26 of RSRC2_HS) stays the DRIVER's field: the merged
     * config cannot carry it because the size depends on the patch count, a
     * pipeline property. The compiler publishes the byte count instead. */
    assert(((hs_rsrc2->value>>18)&0x1ffu)==0u);
    assert(m->hull_tess_wg_valid);
    assert(m->hull_num_patches_per_wg>0 && m->hull_num_patches_per_wg<=255);
    assert(m->hull_tcs_lds_size>0);
    assert(m->hull_workgroup_size>0);
    /* The hull/domain interface state the control stage fully determines:
     * triangles, integer spacing, clockwise output for this fixture. */
    const PsbcRegisterWrite *tf=find_register(m,m->context_registers,
        m->context_register_count,0x2db);
    assert(tf && tf->value==0x41u);
    /* The merged program shares one argument block across both halves. */
    assert(m->user_sgpr_count>0);
    assert(!m->descriptor_set0_valid && !m->push_constants_valid);
    /* The generic loader still refuses it - a hull is not a plain pre-raster
     * program - but the HULL loader accepts exactly this shape. */
    struct ps5vk_runtime_shader arena;
    assert(ps5vk_runtime_shader_build(&arena,&hull)!=0);
    assert(ps5vk_runtime_hull_build(&arena,&hull)==0);
    psbc_free_output(&hull);

    /* The evaluation half compiles, but a plain evaluation compile publishes
     * nothing a driver could launch: no hardware stage, no registers, and the
     * unresolved bit. The loadable form is the NGG one below. */
    PsbcCompileOptions eval_options={
        .target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_TESS_EVAL,.entrypoint="main",
        .optimise=true,.address32_hi=2,.primitive_type=4,
        .rasterization_samples=1};
    PsbcShaderOutput eval={0};
    assert(psbc_compile_shader(es,en*4,&eval_options,&eval)==PSBC_RESULT_OK);
    const PsbcShaderMetadata *e=&eval.metadata;
    assert(e->version==PSBC_SHADER_METADATA_VERSION);
    assert(e->source_stage==PSBC_STAGE_TESS_EVAL);
    assert(e->hardware_stage==PSBC_HW_STAGE_UNKNOWN);
    assert(e->unresolved_fields & PSBC_UNRESOLVED_TESS_PIPELINE);
    assert(!e->context_register_count && !e->shader_register_count);
    assert(!e->linkage_valid);
    assert(ps5vk_runtime_shader_build(&arena,&eval)!=0);
    psbc_free_output(&eval);

    /* The loadable domain package: with the NGG option the evaluation half
     * compiles into the same shape the vertex NGG path publishes, because the
     * pinned radv lowering runs both stages through ac_nir_lower_ngg_nogs.
     * Measured at the pin this fixture compiles to: the ES-slot program
     * registers with a combined RSRC pair, valid GE linkage, the offchip
     * system SGPRs the tessellation args declare (the window base stays above
     * them), the NGG LDS layout slot and real output semantics - and no
     * unresolved tessellation-pipeline bit, because the hull state this
     * package does not carry is the driver's, not the domain half's. */
    PsbcCompileOptions package_options={
        .target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_TESS_EVAL,.entrypoint="main",
        .optimise=true,.ngg=true,.address32_hi=2,.primitive_type=4,
        .rasterization_samples=1};
    PsbcShaderOutput domain={0};
    assert(psbc_compile_shader(es,en*4,&package_options,&domain)==PSBC_RESULT_OK);
    const PsbcShaderMetadata *d=&domain.metadata;
    assert(d->version==PSBC_SHADER_METADATA_VERSION);
    assert(d->target==PSBC_TARGET_PS5);
    assert(d->source_stage==PSBC_STAGE_TESS_EVAL);
    assert(d->hardware_stage==PSBC_HW_STAGE_NGG);
    assert(d->unresolved_fields==
        (PSBC_UNRESOLVED_PROGRAM_CHECKSUM |
         PSBC_UNRESOLVED_NGG_ESGS_RING_ITEMSIZE));
    assert(domain.machine_code && domain.machine_code_size%4==0);
    const PsbcRegisterWrite *es_rsrc1=find_register(d,
        d->shader_registers,d->shader_register_count,0x8a);
    const PsbcRegisterWrite *es_rsrc2=find_register(d,
        d->shader_registers,d->shader_register_count,0x8b);
    const PsbcRegisterWrite *es_rsrc3=find_register(d,
        d->shader_registers,d->shader_register_count,0x87);
    const PsbcRegisterWrite *es_rsrc4=find_register(d,
        d->shader_registers,d->shader_register_count,0x81);
    assert(es_rsrc1 && es_rsrc1->value);
    assert(es_rsrc2 && es_rsrc2->value);
    assert(es_rsrc3 && es_rsrc4);
    assert(find_register(d,d->shader_registers,d->shader_register_count,
        0xc8) && find_register(d,d->shader_registers,d->shader_register_count,
        0xc9));
    assert(d->linkage_valid);
    assert(d->linkage_ge_cntl.offset && d->linkage_stages_en.offset &&
        d->linkage_user_vgpr_en.offset);
    /* The offchip ring handoff: the domain half reads the hull's outputs
     * through the offchip buffer the driver allocates, and the launch gating
     * counts are published so the driver can prove the program's shape. */
    assert(d->esgs_system_sgprs_valid);
    assert(d->esgs_gs_tg_info_sgpr<d->user_data_window_base);
    assert(d->esgs_merged_wave_info_sgpr<d->user_data_window_base);
    assert(d->user_data_window_base==8);
    assert(d->ngg_lds_layout_valid && d->ngg_lds_layout<=UINT16_MAX);
    assert(d->output_semantic_count>=2);
    /* The driver-side loader now packages the domain half as the pre-raster
     * NGG program it is; the load gate's remaining refusal is the hull's. */
    assert(ps5vk_runtime_shader_build(&arena,&domain)==0);
    psbc_free_output(&domain);

    /* The same evaluation half as a LEGACY hardware vertex shader - radv's
     * "Tessellation Evaluation Shader as VS": linked against the control half
     * like the NGG package, NGG off. The publication is the VS block with the
     * gfx10 resource registers radv's preamble writes, the legacy vertex
     * context state plus the three registers a legacy pipeline must clear,
     * the stage enables of a legacy tessellation pipeline (LS, HS, the domain
     * on the VS stage, DYNAMIC_HS, the gfx9+ primgroup bound; wave64 so no
     * W32 bit), and a user-data window at zero: a hardware VS has no system
     * SGPR preamble. radv_postprocess_config's TES-as-VS facts must be in the
     * resource pair: OC_LDS_EN for the off-chip patch data, and at least two
     * VGPR components for the tessellation coordinates. */
    PsbcCompileOptions legacy_options=package_options;
    legacy_options.ngg=false;
    legacy_options.patch_control_points=options.patch_control_points;
    PsbcShaderOutput legacy={0};
    assert(psbc_compile_domain_pipeline(hs,hn*4,es,en*4,&legacy_options,&legacy)==
        PSBC_RESULT_OK);
    const PsbcShaderMetadata *l=&legacy.metadata;
    assert(l->source_stage==PSBC_STAGE_TESS_EVAL);
    assert(l->hardware_stage==PSBC_HW_STAGE_VERTEX);
    assert(l->unresolved_fields==PSBC_UNRESOLVED_PROGRAM_CHECKSUM);
    assert(legacy.machine_code && legacy.machine_code_size%4==0);
    const PsbcRegisterWrite *ld_lo=find_register(l,l->shader_registers,l->shader_register_count,0x48);
    const PsbcRegisterWrite *ld_hi=find_register(l,l->shader_registers,l->shader_register_count,0x49);
    const PsbcRegisterWrite *ld_rsrc1=find_register(l,l->shader_registers,l->shader_register_count,0x4a);
    const PsbcRegisterWrite *ld_rsrc2=find_register(l,l->shader_registers,l->shader_register_count,0x4b);
    assert(ld_lo && ld_hi && !ld_lo->value && !ld_hi->value);
    assert(ld_rsrc1 && ld_rsrc2 && ld_rsrc1->value && ld_rsrc2->value);
    assert(((ld_rsrc1->value>>24)&3u)>=2u);       /* VGPR_COMP_CNT (bits 24..25): tess coords */
    assert(ld_rsrc2->value&(1u<<7));              /* OC_LDS_EN */
    assert(find_register(l,l->shader_registers,l->shader_register_count,0x46));
    assert(find_register(l,l->shader_registers,l->shader_register_count,0x41));
    assert(find_register(l,l->shader_registers,l->shader_register_count,0x47));
    assert(!find_register(l,l->shader_registers,l->shader_register_count,0xc8));
    assert(!find_register(l,l->shader_registers,l->shader_register_count,0x8a));
    assert(l->linkage_valid);
    assert(l->linkage_stages_en.offset==0x2d5);
    assert(l->linkage_stages_en.value==0x10145u);
    assert(l->linkage_ge_cntl.offset==0x25b);
    static const unsigned legacy_cx[]={0x1b1u,0x1c3u,0x207u,0x290u,0x2adu,0x2a1u};
    for(unsigned i=0;i<sizeof(legacy_cx)/sizeof(legacy_cx[0]);++i)
        assert(find_register(l,l->context_registers,l->context_register_count,legacy_cx[i]));
    assert(find_register(l,l->context_registers,l->context_register_count,0x290)->value==0);
    assert(!find_register(l,l->context_registers,l->context_register_count,0x291));
    assert(!l->user_data_window_base);
    assert(!l->esgs_system_sgprs_valid && !l->ngg_lds_layout_valid);
    assert(l->output_semantic_count>=2);
    psbc_free_output(&legacy);

    /* The tessellation pipeline entry point is stage-checked: any other stage
     * option is refused before anything compiles. */
    PsbcCompileOptions wrong=options;
    wrong.stage=PSBC_STAGE_VERTEX;
    PsbcShaderOutput bad={0};
    assert(psbc_compile_tess_pipeline(vs,vn*4,hs,hn*4,es,en*4,&wrong,&bad)!=
        PSBC_RESULT_OK);
    assert(!bad.machine_code && !bad.data);
    psbc_free_output(&bad);

    free(vs);free(hs);free(es);
    psbc_free_output(&standalone_vs);
    puts("Tessellation compiler: hull/domain contract at the pinned dependency described");
    return 0;
}

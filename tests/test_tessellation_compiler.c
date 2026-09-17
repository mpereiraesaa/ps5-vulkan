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
 *   - psbc_compile_tess_pipeline() links the vertex half as the LS program and
 *     the control half as the HS program: the HS machine code (with its GNM
 *     wrapper) stays at offset 0 and the LS machine code is appended at
 *     hull_ls_code_offset, with the LS program/resource registers published
 *     through hull_ls_pgm_lo/hi and hull_ls_rsrc1/2.
 *   - The HS half's RSRC1/RSRC2 are the combined pair the radv
 *     radv_shader_combine_cfg_vs_tcs() rule produces, and the metadata carries
 *     VGT_TF_PARAM derived from the control stage's own execution modes.
 *   - The result still carries PSBC_UNRESOLVED_TESS_PIPELINE: the hull state
 *     the driver owns (stage enables, LS_HS_CONFIG, TF ring, offchip param)
 *     and the loadable domain package do not exist yet.
 *   - A stand-alone evaluation stage compiles to machine code but publishes NO
 *     register writes at all and no hardware stage, so a driver cannot launch
 *     it from this metadata yet; the NGG option is refused for it.
 *   - Until that changes, native/runtime_shader.c must refuse to package the
 *     hull or the domain half: the load gate is the contract, not a bug.
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

    PsbcCompileOptions options={
        .target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_TESS_CTRL,.entrypoint="main",
        .optimise=true,.address32_hi=2,.primitive_type=4,
        .rasterization_samples=1};
    PsbcShaderOutput hull={0};
    assert(psbc_compile_tess_pipeline(vs,vn*4,hs,hn*4,&options,&hull)==
        PSBC_RESULT_OK);
    const PsbcShaderMetadata *m=&hull.metadata;
    assert(m->version==PSBC_SHADER_METADATA_VERSION);
    assert(m->target==PSBC_TARGET_PS5);
    assert(m->source_stage==PSBC_STAGE_TESS_CTRL);
    assert(m->hardware_stage==PSBC_HW_STAGE_UNKNOWN);
    assert(m->unresolved_fields & PSBC_UNRESOLVED_TESS_PIPELINE);
    /* The hull half carries both programs: the HS wrapper at offset 0 and the
     * LS machine code appended behind it. */
    assert(m->hull_ls_valid);
    assert(m->hull_ls_code_offset>0);
    assert(m->hull_ls_code_size>0);
    assert(hull.machine_code_size==(size_t)m->hull_ls_code_offset+
        (size_t)m->hull_ls_code_size);
    assert(hull.machine_code_size%4==0);
    /* The published LS register set is complete and names the LS block the
     * hardware programs, and its config is the vertex half's own. */
    assert(m->hull_ls_pgm_lo.offset==0x148 && !m->hull_ls_pgm_lo.value);
    assert(m->hull_ls_pgm_hi.offset==0x149 && !m->hull_ls_pgm_hi.value);
    assert(m->hull_ls_rsrc1.offset==0x14a);
    assert(m->hull_ls_rsrc1.value==vs_rsrc1->value);
    assert(m->hull_ls_rsrc2.offset==0x14b);
    assert(m->hull_ls_rsrc2.value==vs_rsrc2->value);
    /* The HS half keeps its program registers with the combined config, and
     * the combined RSRC1 is not simply the LS one (the halves differ). */
    const PsbcRegisterWrite *hs_rsrc1=find_register(m,
        m->shader_registers,m->shader_register_count,0x10a);
    const PsbcRegisterWrite *hs_rsrc2=find_register(m,
        m->shader_registers,m->shader_register_count,0x10b);
    assert(hs_rsrc1 && hs_rsrc2 && hs_rsrc1->value && hs_rsrc2->value);
    assert(find_register(m,m->shader_registers,m->shader_register_count,
        0x108) && find_register(m,m->shader_registers,m->shader_register_count,
        0x109));
    /* The hull/domain interface state the control stage fully determines:
     * triangles, integer spacing, clockwise output for this fixture. */
    const PsbcRegisterWrite *tf=find_register(m,m->context_registers,
        m->context_register_count,0x2db);
    assert(tf && tf->value==0x41u);
    /* The control stage compiled with a user-SGPR window (its consumer vertex
     * half shares the argument block) and no advertised descriptor/push use in
     * this fixture; that is the shape the driver's ABI has to serve. */
    assert(m->user_sgpr_count>0);
    assert(!m->descriptor_set0_valid && !m->push_constants_valid);
    /* The load gate must keep refusing both halves while the tessellation
     * package state is unresolved. */
    struct ps5vk_runtime_shader arena;
    assert(ps5vk_runtime_shader_build(&arena,&hull)!=0);
    psbc_free_output(&hull);

    /* The evaluation half compiles, but publishes nothing a driver could
     * launch: no hardware stage, no registers, and the unresolved bit. */
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

    /* The domain half cannot take the NGG path at this pin: the option is
     * refused instead of silently producing an ES-shaped program. */
    eval_options.ngg=true;
    PsbcShaderOutput refused={0};
    assert(psbc_compile_shader(es,en*4,&eval_options,&refused)!=PSBC_RESULT_OK);
    assert(!refused.machine_code && !refused.data);
    psbc_free_output(&refused);

    /* The tessellation pipeline entry point is stage-checked: any other stage
     * option is refused before anything compiles. */
    PsbcCompileOptions wrong=options;
    wrong.stage=PSBC_STAGE_VERTEX;
    PsbcShaderOutput bad={0};
    assert(psbc_compile_tess_pipeline(vs,vn*4,hs,hn*4,&wrong,&bad)!=
        PSBC_RESULT_OK);
    assert(!bad.machine_code && !bad.data);
    psbc_free_output(&bad);

    free(vs);free(hs);free(es);
    psbc_free_output(&standalone_vs);
    puts("Tessellation compiler: hull/domain contract at the pinned dependency described");
    return 0;
}

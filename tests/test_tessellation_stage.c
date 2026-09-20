/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host contract for the tessellation stage pair.
 *
 * This is the interface and identity axis: the stages are described, their
 * execution modes are read, the patch control points the pipeline states are
 * checked against the control stage's output vertex count, and two pipelines
 * that differ only in the pair or in that count are different programs. It says
 * nothing about compiling or executing them. Interface acceptance alone must
 * never be used to promote the tessellationShader capability.
 *
 * The fixtures are the pinned front end's own output for the pair, and each
 * negative case changes exactly one word of it.
 */
#include "spirv_graphics_interface.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct ps5vk_graphics_module_key read_module(const char *path)
{
    FILE *f=fopen(path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>0 && bytes%4==0);
    rewind(f);uint32_t *code=malloc((size_t)bytes);assert(code);
    assert(fread(code,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    return (struct ps5vk_graphics_module_key){.words=code,.word_count=(size_t)bytes/4,.entry="main"};
}

static void free_module(struct ps5vk_graphics_module_key *m)
{ free((void *)m->words);m->words=NULL;m->word_count=0; }

/* Rewrite the value of the single OpDecorate with `decoration` whose value is
 * `from`. The fixtures give every decorated variable a distinct value, so a
 * case that changes one decoration cannot change another interface slot. */
static int patch_decoration(struct ps5vk_graphics_module_key *m,unsigned decoration,
                            uint32_t from,uint32_t to)
{
    uint32_t *words=(uint32_t *)m->words;size_t patched=0;
    for(size_t at=5;at<m->word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;
        if((w[0]&65535u)!=71u || (w[0]>>16)!=4u || w[2]!=decoration || w[3]!=from)continue;
        w[3]=to;++patched;
    }
    return patched==1;
}

/* Rewrite the literal operand of the single OpExecutionMode with `mode`, which
 * is how the control stage's output vertex count is stated. */
static int patch_execution_mode(struct ps5vk_graphics_module_key *m,unsigned mode,
                                uint32_t from,uint32_t to)
{
    uint32_t *words=(uint32_t *)m->words;size_t patched=0;
    for(size_t at=5;at<m->word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;
        if((w[0]&65535u)!=16u || (w[0]>>16)!=4u || w[2]!=mode || w[3]!=from)continue;
        w[3]=to;++patched;
    }
    return patched==1;
}

static struct ps5vk_graphics_key tessellation_key(void)
{
    return (struct ps5vk_graphics_key){
        .vertex=read_module("build/runtime-graphics/tess.vert.spv"),
        .tess_control=read_module("build/runtime-graphics/tess.tesc.spv"),
        .tess_eval=read_module("build/runtime-graphics/tess.tese.spv"),
        .fragment=read_module("build/runtime-graphics/tess.frag.spv"),
        .patch_control_points=3,
        .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
}

static void free_key(struct ps5vk_graphics_key *key)
{
    free_module(&key->vertex);free_module(&key->tess_control);
    free_module(&key->tess_eval);free_module(&key->fragment);
}

int main(void)
{
    /* TES outputs have no implicit outer per-vertex array. All31 vec4
     * locations must reach FS; stripping the array formerly left only loc0. */
    struct ps5vk_graphics_key envelope=tessellation_key();
    free_module(&envelope.tess_eval);free_module(&envelope.fragment);
    envelope.tess_eval=read_module("build/runtime-graphics/tess_output_envelope.tese.spv");
    envelope.fragment=read_module("build/runtime-graphics/tess_output_envelope.frag.spv");
    assert(ps5vk_spirv_graphics_interface(&envelope));
    assert(patch_decoration(&envelope.tess_eval,30,0,1));
    assert(!ps5vk_spirv_graphics_interface(&envelope));
    free_key(&envelope);
    /* The pair the front end emits for a three-control-point patch: the control
     * stage declares OutputVertices 3, the evaluation stage declares its
     * domain, spacing and winding, and the per-patch value crosses through the
     * Patch-decorated interface rather than the per-vertex one. */
    struct ps5vk_graphics_key key=tessellation_key();
    assert(ps5vk_graphics_has_tessellation(&key));
    assert(ps5vk_graphics_tessellation_key_valid(&key));
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_spirv_tess_output_points(&key.tess_control)==3);
    assert(ps5vk_spirv_tess_output_points(&key.tess_eval)==0);

    /* Reflection-only execution-mode mutations: verify the primitive boundary
     * for every TES domain, including point-mode precedence. Not GPU evidence. */
    const unsigned domains[]={22,24,25};
    const char *geometry_paths[]={"build/runtime-graphics/geometry_points.geom.spv",
        "build/runtime-graphics/geometry_lines.geom.spv",
        "build/runtime-graphics/geometry_probe.geom.spv"};
    for(unsigned domain=0;domain<3;++domain)for(unsigned point=0;point<2;++point) {
        struct ps5vk_graphics_module_key eval=read_module("build/runtime-graphics/tess.tese.spv");
        uint32_t *words=(uint32_t *)eval.words,entry=0;
        for(size_t at=5;at<eval.word_count;at+=words[at]>>16) {
            if((words[at]&65535u)==16u && (words[at]>>16)==3u && words[at+2]==22) {
                words[at+2]=domains[domain];entry=words[at+1];
            }
        }
        assert(entry);
        if(point) {
            words=realloc(words,(eval.word_count+3)*sizeof(*words));assert(words);
            words[eval.word_count++]=(3u<<16)|16u;
            words[eval.word_count++]=entry;words[eval.word_count++]=10u;
            eval.words=words;
        }
        for(unsigned primitive=0;primitive<3;++primitive) {
            struct ps5vk_graphics_module_key geom=read_module(geometry_paths[primitive]);
            struct ps5vk_graphics_key five=key;five.tess_eval=eval;five.geometry=geom;
            const unsigned expected=point?0u:domain==2?1u:2u;
            assert(!!ps5vk_spirv_graphics_interface(&five)==(primitive==expected));
            free_module(&geom);
        }
        free_module(&eval);
    }

    /* PrimitiveId is a patch index input in TCS, not a geometry-only input.
     * Reuse the scalar input declaration, changing only its BuiltIn. */
    struct ps5vk_graphics_module_key primitive=
        read_module("build/runtime-graphics/tess.tesc.spv");
    assert(patch_decoration(&primitive,11u,8u,7u));
    struct ps5vk_graphics_key primitive_key=key;
    primitive_key.tess_control=primitive;
    assert(ps5vk_spirv_graphics_interface(&primitive_key));
    assert(patch_decoration(&primitive,11u,7u,14u));
    assert(ps5vk_spirv_graphics_interface(&primitive_key));
    /* A different, unsupported builtin must not be accepted by this change. */
    assert(patch_decoration(&primitive,11u,14u,999u));
    assert(!ps5vk_spirv_graphics_interface(&primitive_key));
    free_module(&primitive);

    /* Input assembly size and TCS output size are independent. Out-of-range
     * input counts are still refused before interface work. */
    struct ps5vk_graphics_key wrong=key;
    wrong.patch_control_points=4;
    assert(ps5vk_graphics_tessellation_key_valid(&wrong));
    assert(ps5vk_spirv_graphics_interface(&wrong));
    wrong.patch_control_points=0;
    assert(!ps5vk_graphics_tessellation_key_valid(&wrong));
    assert(!ps5vk_spirv_graphics_interface(&wrong));
    wrong.patch_control_points=33;
    assert(!ps5vk_graphics_tessellation_key_valid(&wrong));
    assert(!ps5vk_spirv_graphics_interface(&wrong));

    /* Half a pair is not a tessellation pipeline: Vulkan requires the control
     * and evaluation stages together, and the key contract encodes it. */
    struct ps5vk_graphics_key half=key;
    half.tess_eval=(struct ps5vk_graphics_module_key){0};
    assert(ps5vk_graphics_has_tessellation(&half));
    assert(!ps5vk_graphics_tessellation_key_valid(&half));
    assert(!ps5vk_spirv_graphics_interface(&half));

    /* The stages are identified by their execution models: feeding the control
     * module where the evaluation stage belongs is not the pair the compiler
     * would build. */
    struct ps5vk_graphics_key swapped=key;
    swapped.tess_control=key.tess_eval;
    swapped.tess_eval=key.tess_control;
    assert(!ps5vk_spirv_graphics_interface(&swapped));

    /* Both expansion (3 -> 32) and equal sizes are valid interfaces. These
     * reflected module mutations test declarations, not shader execution. */
    struct ps5vk_graphics_module_key widened=
        read_module("build/runtime-graphics/tess.tesc.spv");
    assert(patch_execution_mode(&widened,26,3,32));
    struct ps5vk_graphics_key wide=key;
    wide.tess_control=widened;
    wide.patch_control_points=32;
    assert(ps5vk_spirv_graphics_interface(&wide));
    wide.patch_control_points=3;
    assert(ps5vk_spirv_graphics_interface(&wide));
    assert(ps5vk_spirv_tess_output_points(&wide.tess_control)==32);
    assert(patch_execution_mode(&widened,26,32,0));
    assert(!ps5vk_spirv_graphics_interface(&wide));
    assert(ps5vk_spirv_tess_output_points(&wide.tess_control)==0);
    assert(patch_execution_mode(&widened,26,0,33));
    assert(!ps5vk_spirv_graphics_interface(&wide));
    free_module(&widened);

    /* An evaluation stage whose domain is a mode the profile does not know is
     * refused rather than defaulted: the domain decides the tessellator's
     * patches and the primitive the domain stage emits. */
    struct ps5vk_graphics_module_key no_domain=
        read_module("build/runtime-graphics/tess.tese.spv");
    {
        uint32_t *words=(uint32_t *)no_domain.words;size_t patched=0;
        for(size_t at=5;at<no_domain.word_count;at+=words[at]>>16) {
            uint32_t *w=words+at;
            if((w[0]&65535u)==16u && (w[0]>>16)==3u && w[2]==22) { w[2]=99;++patched; }
        }
        assert(patched==1);
    }
    struct ps5vk_graphics_key unknown=key;
    unknown.tess_eval=no_domain;
    assert(!ps5vk_spirv_graphics_interface(&unknown));
    free_module(&no_domain);

    /* The per-patch interface crosses the pair: a control stage that writes its
     * per-patch value at another location has not written the one the
     * evaluation stage reads. */
    struct ps5vk_graphics_module_key moved=read_module("build/runtime-graphics/tess.tesc.spv");
    assert(patch_decoration(&moved,30,1,2));
    struct ps5vk_graphics_key moved_key=key;
    moved_key.tess_control=moved;
    assert(!ps5vk_spirv_graphics_interface(&moved_key));
    free_module(&moved);

    /* SPIRV-Tools #5654: Patch/non-Patch locations are independent. Start
     * from actual frontend modules and relocate only the patch variables;
     * our GLSL frontend cannot express this alias directly. Reflection-only,
     * not a claim that this mutated shader was validated on hardware. */
    struct ps5vk_graphics_module_key alias_control=
        read_module("build/runtime-graphics/tess.tesc.spv");
    struct ps5vk_graphics_module_key alias_eval=
        read_module("build/runtime-graphics/tess.tese.spv");
    assert(patch_decoration(&alias_control,30,1,0));
    assert(patch_decoration(&alias_eval,30,1,0));
    struct ps5vk_graphics_key alias_key=key;
    alias_key.tess_control=alias_control;
    alias_key.tess_eval=alias_eval;
    assert(ps5vk_spirv_graphics_interface(&alias_key));
    /* Independence is not permission to drop producer/consumer matching. */
    alias_key.tess_eval=key.tess_eval;
    assert(!ps5vk_spirv_graphics_interface(&alias_key));
    free_module(&alias_control);
    free_module(&alias_eval);

    /* Distances are a pre-raster export, and with a tessellation pair the last
     * pre-raster stage is one of its halves: the declaration bound has to read
     * them from there as well. */
    unsigned clip=~0u,cull=~0u;
    assert(ps5vk_spirv_stage_distance_declarations(&key.tess_control,&clip,&cull));
    assert(clip==1 && cull==1);
    assert(ps5vk_spirv_stage_distance_declarations(&key.tess_eval,&clip,&cull));
    assert(clip==1 && cull==1);

    free_key(&key);
    puts("Tessellation stage: pair identity, execution modes and patch control points described");
    return 0;
}

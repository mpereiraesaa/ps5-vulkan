/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host contract for the clip/cull distance declaration policy.
 *
 * This is the interface axis only. Accepting a declaration here says nothing
 * about compiling it, creating a pipeline from it or executing it on the GPU;
 * those remain separate gates (the native metadata adapter, pipeline creation
 * and native pixel evidence). The front-end fixtures are the pinned compiler
 * front end's own output for the declarations an application can write, and
 * each negative case changes exactly one word of that output.
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

static uint32_t *mutable_words(const struct ps5vk_graphics_module_key *m)
{ return (uint32_t *)m->words; }

/* Rewrite the literal length of the single OpTypeArray whose length constant is
 * `from`. glslang emits one float[N] type shared by every distance array of
 * that width, so this changes the declared width of each array that uses it and
 * nothing else in the module. */
static void patch_distance_length(struct ps5vk_graphics_module_key *m,
                                 unsigned from,unsigned to)
{
    uint32_t *words=mutable_words(m);size_t patched=0;
    for(size_t at=5;at<m->word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;unsigned n=w[0]>>16,op=w[0]&65535;
        if(op!=28 || n!=4)continue;                       /* OpTypeArray */
        size_t length_id=w[3];
        for(size_t literal=5;literal<m->word_count;literal+=words[literal]>>16) {
            uint32_t *c=words+literal;
            if((c[0]&65535)!=43 || (c[0]>>16)!=4 || c[2]!=length_id || c[3]!=from)continue;
            c[3]=to;++patched;
        }
    }
    assert(patched==1);
}

/* Rewrite the storage class of the two distance variables of a built module, so
 * a case can describe the form no legal front end emits: a fragment stage that
 * would WRITE a distance. */
static void patch_distance_storage(struct ps5vk_graphics_module_key *m,unsigned storage)
{
    uint32_t *words=mutable_words(m);size_t patched=0;
    for(size_t at=5;at<m->word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;unsigned n=w[0]>>16,op=w[0]&65535;
        if(op!=59 || n!=4)continue;                 /* OpVariable */
        if(w[2]!=6 && w[2]!=7)continue;             /* the distance variables */
        w[3]=storage;++patched;
    }
    assert(patched>=1);
}

/* A module the front end does not produce, written directly so a case can
 * declare exactly the distances the fixtures cannot: a bare
 * gl_ClipDistance/gl_CullDistance variable instead of the gl_PerVertex block, a
 * built-in the profile does not deliver, a fragment-stage declaration, or the
 * combined width ceiling.
 *
 * Both distance variables share one float[length] type, as a front end that
 * emits SPIR-V directly would, so `clip_length` and `cull_length` are each
 * either zero or `length`. `color_output` adds the location-0 vec4 fragment
 * output the pipeline check requires, which lets a fragment-stage case fail for
 * its distance declaration alone. */
enum { MODULE_WORDS = 96, MODULE_BOUND = 17 };
static void build_module(uint32_t *words,unsigned length,
                        unsigned clip_length,unsigned cull_length,
                        uint32_t clip_builtin,uint32_t cull_builtin,
                        uint32_t model,int color_output,
                        struct ps5vk_graphics_module_key *out)
{
    assert(color_output==0 || color_output==1);
    assert((clip_length==0 && cull_length==0) || length>=1);
    assert((clip_length==0 || clip_length==length) &&
           (cull_length==0 || cull_length==length));
    /* A fragment module cannot write a distance, so its declaration is an
     * input; every pre-raster model writes them, so those stay outputs. */
    const unsigned storage=model==4?1u:3u;
    unsigned interfaces=(clip_length?1u:0u)+(cull_length?1u:0u)+(color_output?1u:0u);
    size_t at=0;
    words[at++]=0x07230203u;words[at++]=0x00010000u;words[at++]=0u;
    words[at++]=MODULE_BOUND;words[at++]=0u;
    words[at++]=(2u<<16)|17u;words[at++]=1u;               /* OpCapability Shader */
    words[at++]=(3u<<16)|14u;words[at++]=0u;words[at++]=1u; /* OpMemoryModel Logical GLSL450 */
    /* OpEntryPoint <model> %13 "main" and the interface variables */
    words[at++]=(uint32_t)(((5u+interfaces)<<16)|15u);
    words[at++]=model;words[at++]=13u;words[at++]=0x6e69616du;words[at++]=0u;
    if(clip_length)words[at++]=6u;
    if(cull_length)words[at++]=7u;
    if(color_output)words[at++]=10u;
    if(clip_length) {
        words[at++]=(4u<<16)|71u;words[at++]=6u;words[at++]=11u;words[at++]=clip_builtin;
    }
    if(cull_length) {
        words[at++]=(4u<<16)|71u;words[at++]=7u;words[at++]=11u;words[at++]=cull_builtin;
    }
    if(color_output) {
        words[at++]=(4u<<16)|71u;words[at++]=10u;words[at++]=30u;words[at++]=0u; /* Location 0 */
    }
    words[at++]=(3u<<16)|22u;words[at++]=1u;words[at++]=32u; /* OpTypeFloat %1 32 */
    words[at++]=(4u<<16)|21u;words[at++]=2u;words[at++]=32u;words[at++]=0u; /* OpTypeInt %2 32 0 */
    if(clip_length || cull_length) {
        words[at++]=(4u<<16)|43u;words[at++]=2u;words[at++]=3u;words[at++]=length; /* OpConstant */
        words[at++]=(4u<<16)|28u;words[at++]=4u;words[at++]=1u;words[at++]=3u; /* OpTypeArray */
        words[at++]=(4u<<16)|32u;words[at++]=5u;words[at++]=storage;words[at++]=4u; /* OpTypePointer */
        words[at++]=(4u<<16)|59u;words[at++]=5u;words[at++]=6u;words[at++]=storage; /* OpVariable %6 */
        if(cull_length)
            { words[at++]=(4u<<16)|59u;words[at++]=5u;words[at++]=7u;words[at++]=storage; }
    }
    words[at++]=(4u<<16)|23u;words[at++]=8u;words[at++]=1u;words[at++]=4u; /* OpTypeVector %8 %1 4 */
    words[at++]=(4u<<16)|32u;words[at++]=9u;words[at++]=3u;words[at++]=8u; /* OpTypePointer %9 Output %8 */
    if(color_output) {
        words[at++]=(4u<<16)|59u;words[at++]=9u;words[at++]=10u;words[at++]=3u;
    }
    words[at++]=(2u<<16)|19u;words[at++]=11u;              /* OpTypeVoid %11 */
    words[at++]=(3u<<16)|33u;words[at++]=12u;words[at++]=11u; /* OpTypeFunction %12 %11 */
    words[at++]=(5u<<16)|54u;words[at++]=11u;words[at++]=13u;words[at++]=0u;words[at++]=12u;
    words[at++]=(2u<<16)|248u;words[at++]=14u;             /* OpLabel %14 */
    words[at++]=(1u<<16)|253u;                             /* OpReturn */
    words[at++]=(1u<<16)|56u;                              /* OpFunctionEnd */
    assert(at<MODULE_WORDS);
    *out=(struct ps5vk_graphics_module_key){.words=words,.word_count=at,.entry="main"};
}

static struct ps5vk_graphics_key staged_key(struct ps5vk_graphics_module_key vertex,
                                           struct ps5vk_graphics_module_key fragment)
{
    return (struct ps5vk_graphics_key){.vertex=vertex,.fragment=fragment,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
}

int main(void)
{
    struct ps5vk_graphics_module_key plain_fragment=
        read_module("build/runtime-graphics/triangle.frag.spv");
    struct ps5vk_graphics_module_key triangle=
        read_module("build/runtime-graphics/triangle.vert.spv");
    uint32_t module_words[MODULE_WORDS],other_words[MODULE_WORDS],
        fragment_words[MODULE_WORDS],swapped_words[MODULE_WORDS],
        export_words[MODULE_WORDS];
    struct ps5vk_graphics_module_key plain_vertex,fragment_only,declares_in_fragment;
    unsigned clip=~0u,cull=~0u,other_clip=~0u,other_cull=~0u;

    /* The fragment module the hand-written vertex modules pair with: it reads
     * nothing and writes one colour, so a rejection cannot come from the
     * varying interface. */
    build_module(fragment_words,0,0,0,3,3,4,1,&fragment_only);

    /* The VS+FS profile that already works declares the front end's default
     * one-element clip and cull arrays and writes neither: a declaration is not
     * usage, so the bound check keeps accepting what the compiler will not
     * export. */
    assert(ps5vk_spirv_stage_distance_declarations(&triangle,&clip,&cull));
    assert(clip==1 && cull==1);
    struct ps5vk_graphics_key plain=staged_key(triangle,plain_fragment);
    assert(ps5vk_spirv_graphics_interface(&plain));

    /* gl_PerVertex redeclared with a clip distance member: the form the pinned
     * front end emits for an application that writes gl_ClipDistance. */
    struct ps5vk_graphics_module_key clip_module=
        read_module("build/runtime-graphics/clip_distance.vert.spv");
    clip=~0u;cull=~0u;
    assert(ps5vk_spirv_stage_distance_declarations(&clip_module,&clip,&cull));
    assert(clip==2 && cull==0);
    struct ps5vk_graphics_key clip_key=staged_key(clip_module,plain_fragment);
    assert(ps5vk_spirv_graphics_interface(&clip_key));

    struct ps5vk_graphics_module_key cull_module=
        read_module("build/runtime-graphics/cull_distance.vert.spv");
    clip=~0u;cull=~0u;
    assert(ps5vk_spirv_stage_distance_declarations(&cull_module,&clip,&cull));
    assert(clip==0 && cull==1);
    struct ps5vk_graphics_key cull_key=staged_key(cull_module,plain_fragment);
    assert(ps5vk_spirv_graphics_interface(&cull_key));

    /* The profile ceiling: four clip plus four cull components fill both packed
     * position registers past POS0. */
    struct ps5vk_graphics_module_key both_module=
        read_module("build/runtime-graphics/clip_cull_distance.vert.spv");
    clip=~0u;cull=~0u;
    assert(ps5vk_spirv_stage_distance_declarations(&both_module,&clip,&cull));
    assert(clip==4 && cull==4);
    struct ps5vk_graphics_key both_key=staged_key(both_module,plain_fragment);
    assert(ps5vk_spirv_graphics_interface(&both_key));

    /* A declaration wider than the stage can export is refused, and the
     * unusable module reports no counts rather than a half-valid pair. */
    patch_distance_length(&clip_module,2,9);
    clip=~0u;cull=~0u;
    assert(!ps5vk_spirv_stage_distance_declarations(&clip_module,&clip,&cull));
    assert(clip==0 && cull==0);
    clip_key.vertex=clip_module;
    assert(!ps5vk_spirv_graphics_interface(&clip_key));

    /* Widening the shared distance type widens both declarations, so the pair
     * that was exactly at the ceiling is now past it: each feature stays inside
     * its own floor while the combined count does not. */
    patch_distance_length(&both_module,4,5);
    assert(!ps5vk_spirv_graphics_interface(&both_key));

    /* A bare gl_ClipDistance variable is the other legal SPIR-V form and is
     * accepted at the declared width. */
    struct ps5vk_graphics_module_key bare,cull_declares,combined;
    build_module(module_words,2,2,0,3,3,0,0,&bare);
    assert(ps5vk_spirv_stage_distance_declarations(&bare,&clip,&cull));
    assert(clip==2 && cull==0);
    struct ps5vk_graphics_key bare_key=staged_key(bare,fragment_only);
    assert(ps5vk_spirv_graphics_interface(&bare_key));

    build_module(other_words,3,0,3,3,4,0,0,&cull_declares);
    assert(ps5vk_spirv_stage_distance_declarations(&cull_declares,&other_clip,&other_cull));
    assert(other_clip==0 && other_cull==3);
    struct ps5vk_graphics_key cull_declares_key=staged_key(cull_declares,fragment_only);
    assert(ps5vk_spirv_graphics_interface(&cull_declares_key));

    /* Eight clip plus eight cull components satisfies each feature's own floor
     * and still cannot be exported: the two shared registers hold eight. */
    build_module(module_words,8,8,8,3,4,0,0,&combined);
    struct ps5vk_graphics_key combined_key=staged_key(combined,fragment_only);
    assert(!ps5vk_spirv_graphics_interface(&combined_key));

    /* A built-in the profile does not deliver is refused in the pre-raster
     * form, and the same module is not a fragment interface either. */
    struct ps5vk_graphics_module_key unknown;
    build_module(module_words,2,2,0,15,3,0,0,&unknown);
    assert(!ps5vk_spirv_stage_distance_declarations(&unknown,NULL,NULL));
    assert(!ps5vk_spirv_stage_distance_reads(&unknown,NULL,NULL));

    /* The fragment stage declares the same two built-ins as PIXEL INPUTS. A
     * read the last pre-raster stage exports is accepted and reported
     * separately from an export; a read the producer never wrote is refused;
     * and a fragment stage that would write a distance is refused. */
    build_module(swapped_words,2,2,0,3,4,4,1,&declares_in_fragment);
    assert(!ps5vk_spirv_stage_distance_declarations(&declares_in_fragment,NULL,NULL));
    unsigned read_clip=~0u,read_cull=~0u;
    assert(ps5vk_spirv_stage_distance_reads(&declares_in_fragment,&read_clip,&read_cull));
    assert(read_clip==2 && read_cull==0);
    build_module(module_words,0,0,0,3,3,0,0,&plain_vertex);
    struct ps5vk_graphics_key swapped=staged_key(plain_vertex,declares_in_fragment);
    /* The producer exports nothing, so the read cannot be delivered. */
    assert(!ps5vk_spirv_graphics_interface(&swapped));
    /* A fresh producer: clip_module's shared distance type was widened past the
     * ceiling earlier, so it can no longer be the exporting half of a key. */
    struct ps5vk_graphics_module_key exports_clip;
    build_module(export_words,2,2,0,3,3,0,0,&exports_clip);
    assert(ps5vk_spirv_stage_distance_declarations(&exports_clip,&clip,&cull));
    assert(clip==2 && cull==0);
    struct ps5vk_graphics_key reads_key=staged_key(exports_clip,declares_in_fragment);
    assert(ps5vk_spirv_graphics_interface(&reads_key));
    /* Three declared reads against two exported components: refused. */
    struct ps5vk_graphics_module_key over_reads;
    build_module(other_words,3,3,0,3,4,4,1,&over_reads);
    assert(ps5vk_spirv_stage_distance_reads(&over_reads,&read_clip,&read_cull));
    assert(read_clip==3 && read_cull==0);
    struct ps5vk_graphics_key over_key=staged_key(exports_clip,over_reads);
    assert(!ps5vk_spirv_graphics_interface(&over_key));
    /* A fragment Output distance is not an interface this profile has. */
    patch_distance_storage(&declares_in_fragment,3);
    assert(!ps5vk_spirv_stage_distance_reads(&declares_in_fragment,NULL,NULL));
    assert(!ps5vk_spirv_graphics_interface(&reads_key));

    /* gl_FragCoord. A core-Vulkan fragment built-in that needs no feature and
     * no export from the pre-raster stage: the hardware launches the pixel
     * stage with the position VGPRs and the compiler asks for them through
     * SPI_PS_INPUT_ENA. The policy used to accept no fragment built-in except
     * ViewIndex, so the only applicable depthClamp leaves
     * (dEQP-VK.clipping.clip_volume.depth_clamp.*, whose fragment shader
     * colours with gl_FragCoord.z) were refused at pipeline creation with
     * VK_ERROR_FEATURE_NOT_PRESENT - measured twice as rc=-8 in the
     * runtime-graphics cache during the 2026-09-20 run. */
    struct ps5vk_graphics_module_key frag_coord=
        read_module("build/runtime-graphics/frag_coord.frag.spv");
    struct ps5vk_graphics_key frag_coord_key=staged_key(triangle,frag_coord);
    assert(ps5vk_spirv_graphics_interface(&frag_coord_key));

    /* The acceptance is bounded to the built-in's own shape: the same
     * declaration as anything other than a fragment input is still refused. */
    {
        uint32_t *words=mutable_words(&frag_coord);
        size_t patched=0;
        for(size_t at=5;at<frag_coord.word_count;at+=words[at]>>16) {
            uint32_t *w=words+at;
            /* OpDecorate <id> BuiltIn FragCoord(15) -> BuiltIn PointSize(1),
             * which no fragment stage may declare as an input. */
            if((w[0]&65535)==71 && (w[0]>>16)==4 && w[2]==11 && w[3]==15) {
                w[3]=1;++patched;
            }
        }
        assert(patched==1);
        struct ps5vk_graphics_key wrong=staged_key(triangle,frag_coord);
        assert(!ps5vk_spirv_graphics_interface(&wrong));
    }
    free_module(&frag_coord);

    free_module(&plain_fragment);free_module(&triangle);free_module(&clip_module);
    free_module(&cull_module);free_module(&both_module);
    puts("Graphics stages: clip/cull exports and pixel reads bounded, a read past the export refused, gl_FragCoord accepted as a fragment input, unusable modules report no counts");
    return 0;
}

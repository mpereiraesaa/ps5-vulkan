#include "runtime_graphics_compiler.h"
#include "compilation_cache.h"
#include "spirv_graphics_interface.h"
#include "resolve_program.h"
#include "vertex_format_probe.h"
#include "texture_format.h"
#include "descriptor_table_layout.h"
#include "vk_descriptor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dualSrcBlend is promoted: the profile serves the whole GFX1013 blend
 * contract, not the single witnessed shape, and the assertions below state the
 * promoted contract. */

static struct ps5vk_graphics_module_key read_module(const char *path)
{
    FILE *f=fopen(path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>0 && bytes%4==0);
    rewind(f);uint32_t *code=malloc((size_t)bytes);assert(code);
    assert(fread(code,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    return (struct ps5vk_graphics_module_key){.words=code,.word_count=(size_t)bytes/4,.entry="main"};
}

/* Single-word patches over the test's OWN copy of a module. The two negatives
 * below are the same SPIR-V with exactly one thing changed - the entry point's
 * execution model, or the built-in a decoration names - so the case says what
 * it refuses and nothing else. */
static int patch_entry_model(struct ps5vk_graphics_module_key *m, uint32_t model)
{
    uint32_t *words=(uint32_t *)m->words;
    for(size_t at=5;at<m->word_count;at+=words[at]>>16) {
        if((words[at]&65535u)==15u && (words[at]>>16)>=4u) { words[at+1]=model; return 1; }
    }
    return 0;
}
static int patch_builtin(struct ps5vk_graphics_module_key *m, uint32_t from, uint32_t to)
{
    uint32_t *words=(uint32_t *)m->words;
    for(size_t at=5;at<m->word_count;at+=words[at]>>16) {
        if((words[at]&65535u)==71u && (words[at]>>16)==4u && words[at+2]==11u &&
           words[at+3]==from) { words[at+3]=to; return 1; }
    }
    return 0;
}

#define PS5VK_TEST_PS_INPUT_ENA_OFFSET  ((uint16_t)((0x0286CCu - 0x00028000u) / 4u))
#define PS5VK_TEST_PS_INPUT_ADDR_OFFSET ((uint16_t)((0x0286D0u - 0x00028000u) / 4u))
#define PS5VK_TEST_POS_Z_FLOAT_ENA      (1u << 10)
#define PS5VK_TEST_POS_XYZW_FLOAT_ENA   (0xfu << 8)
#define PS5VK_TEST_LAUNCH_VGPR_ENA      (0xffu)
#define PS5VK_TEST_SPI_SHADER_COL_FORMAT_OFFSET ((uint16_t)0x1c5u)
#define PS5VK_TEST_CB_SHADER_MASK_OFFSET        ((uint16_t)0x08fu)

static uint32_t published_context_register(const void *pair, uint16_t offset, int *found)
{
    const struct ps5vk_runtime_graphics_program *program=pair;
    *found=0;
    for(unsigned i=0;i<program->fragment.metadata.context_register_count;++i)
        if(program->fragment.metadata.context_registers[i].offset==offset) {
            *found=1;
            return program->fragment.metadata.context_registers[i].value;
        }
    return 0;
}

static uint32_t compiled_ps_input_ena(const char *fragment_path)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module(fragment_path),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *pair=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&pair)==VK_SUCCESS && pair);
    int found_ena=0,found_addr=0;
    const uint32_t ena=published_context_register(pair,PS5VK_TEST_PS_INPUT_ENA_OFFSET,&found_ena);
    const uint32_t addr=published_context_register(pair,PS5VK_TEST_PS_INPUT_ADDR_OFFSET,&found_addr);
    assert(found_ena && found_addr && (ena&addr)==ena);
    ps5vk_runtime_graphics_free(NULL,pair);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    return ena;
}

/* The sample-rate contract the compiler adapter repeats (DXVK262-T06): a count
 * this profile implements compiles, per-sample shading additionally needs the
 * feature the logical device enabled, and a count outside the envelope is
 * refused before any compiler work. The count reaches PSBC as
 * rasterization_samples, which is the option the pinned compiler turns into
 * its per-sample pixel ABI; the fragment metadata below is what proves the
 * option was accepted rather than ignored. */
static void check_sample_rate_compilation(void)
{
    struct ps5vk_graphics_key key={
        /* The witness pair: an oversized triangle that covers the whole target
         * and a fragment module that reads gl_SampleID, which is what makes the
         * compiled program a per-sample one - the interface has to accept the
         * built-in and the compiler has to accept the count together. */
        .vertex=read_module("build/runtime-graphics/sample_id.vert.spv"),
        .fragment=read_module("build/runtime-graphics/sample_id.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    assert(ps5vk_spirv_graphics_interface(&key));
    /* 4x without per-sample shading: the multisample state the front end
     * accepts compiles, and it is a different program from the 1x one. */
    key.samples=VK_SAMPLE_COUNT_4_BIT;
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *quad=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&quad)==VK_SUCCESS && quad);
    ps5vk_runtime_graphics_free(NULL,quad);
    /* Per-sample shading needs sampleRateShading on the logical device, and
     * the fraction the front end accepted. */
    key.sample_shading_enable=VK_TRUE;
    key.min_sample_shading=0.5f;
    assert(!ps5vk_runtime_graphics_supported(&key));
    key.feature_mask|=PS5VK_FEATURE_SAMPLE_RATE_SHADING;
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *shaded=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&shaded)==VK_SUCCESS && shaded);
    ps5vk_runtime_graphics_free(NULL,shaded);
    /* A fraction outside [0,1] is not a state this contract carries, and a
     * count outside the envelope is refused by the adapter itself. */
    key.min_sample_shading=1.5f;
    assert(!ps5vk_runtime_graphics_supported(&key));
    key.min_sample_shading=1.0f;
    key.samples=VK_SAMPLE_COUNT_8_BIT;
    assert(!ps5vk_runtime_graphics_supported(&key));
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

static void check_fragment_position(void)
{
    /* gl_FragCoord needs no export from the pre-raster stage and no feature:
     * the pixel wave is launched with the position VGPRs when SPI_PS_INPUT_ENA
     * asks for them, and the pinned compiler publishes that register with the
     * rest of the pixel context. The profile refused the declaration outright
     * until this slice, which is why the only applicable depthClamp leaves
     * could not run - dEQP-VK.clipping.clip_volume.depth_clamp.* colours with
     * gl_FragCoord.z and the pair was refused at pipeline creation (two rc=-8
     * runtime-graphics cache entries in the 2026-09-20 run, eboot 749756aa).
     * Reading gl_FragCoord.z moves the fixture's interpolation half to
     * LINE_STIPPLE_TEX, the cheapest mandatory enable the compiler can pick
     * when the stage interpolates nothing (radv_shader.c:3971-3984). */
    const uint32_t plain=compiled_ps_input_ena("build/runtime-graphics/triangle.frag.spv");
    assert(!(plain&PS5VK_TEST_POS_XYZW_FLOAT_ENA));
    assert(plain&PS5VK_TEST_LAUNCH_VGPR_ENA);
    const uint32_t position=compiled_ps_input_ena("build/runtime-graphics/frag_coord.frag.spv");
    assert(position&PS5VK_TEST_POS_Z_FLOAT_ENA);
    assert(position&PS5VK_TEST_LAUNCH_VGPR_ENA);
    assert(position!=plain);

    struct ps5vk_graphics_module_key invalid=
        read_module("build/runtime-graphics/frag_coord.frag.spv");
    assert(patch_builtin(&invalid,15u,UINT32_C(0x7ffffff0)));
    struct ps5vk_graphics_key invalid_key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=invalid,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    assert(!ps5vk_spirv_graphics_interface(&invalid_key));
    free((void *)invalid_key.vertex.words);free((void *)invalid_key.fragment.words);
}

/* A second fragment output at Location 0, Index 1 is not another MRT.  The
 * pinned PSBC/ACO epilogue must package both values as the two sources of MRT0:
 * one FP16_ABGR export nibble per source and one RGBA write mask per source.
 * These exact registers are the compiler half of the dualSrcBlend contract;
 * they do not authorize the draw path or advertise the device feature. */
static void check_dual_source_exports(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/dual_source.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .blend_enable[0]=VK_TRUE,
        .src_color_blend_factor[0]=VK_BLEND_FACTOR_SRC_ALPHA,
        .dst_color_blend_factor[0]=VK_BLEND_FACTOR_ONE,
        .color_blend_op[0]=VK_BLEND_OP_ADD,
        .src_alpha_blend_factor[0]=VK_BLEND_FACTOR_SRC_ALPHA,
        .dst_alpha_blend_factor[0]=VK_BLEND_FACTOR_ONE,
        .alpha_blend_op[0]=VK_BLEND_OP_ADD};
    assert(ps5vk_spirv_graphics_interface(&key));
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_FRAGMENT,
        .entrypoint="main",.optimise=true,.address32_hi=2,
        .rasterization_samples=1,.spi_shader_col_format=4};
    PsbcShaderOutput compiled={0};
    assert(psbc_compile_shader(key.fragment.words,key.fragment.word_count*4u,
        &options,&compiled)==PSBC_RESULT_OK);
    int found_format=0,found_mask=0;
    uint32_t spi_format=0,shader_mask=0;
    for(unsigned i=0;i<compiled.metadata.context_register_count;++i) {
        const PsbcRegisterWrite *reg=&compiled.metadata.context_registers[i];
        if(reg->offset==PS5VK_TEST_SPI_SHADER_COL_FORMAT_OFFSET) {
            spi_format=reg->value;found_format=1;
        }
        if(reg->offset==PS5VK_TEST_CB_SHADER_MASK_OFFSET) {
            shader_mask=reg->value;found_mask=1;
        }
    }
    assert(spi_format==UINT32_C(0x44));
    assert(shader_mask==UINT32_C(0xff));
    assert(found_format && found_mask);
    assert(ps5vk_runtime_fragment_export(&compiled.metadata)==
        PS5VK_RUNTIME_FRAGMENT_EXPORT_DUAL);
    {
        PsbcShaderMetadata malformed=compiled.metadata;
        for(unsigned i=0;i<malformed.context_register_count;++i)
            if(malformed.context_registers[i].offset==
               PS5VK_TEST_CB_SHADER_MASK_OFFSET)
                malformed.context_registers[i].value=15u;
        assert(ps5vk_runtime_fragment_export(&malformed)<0);
    }
    psbc_free_output(&compiled);

    const void *runtime=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_SUCCESS && runtime);
    assert(((const struct ps5vk_runtime_graphics_program *)runtime)->dual_source_export==1u);
    ps5vk_runtime_graphics_free(NULL,runtime);
    /* The secondary export is only compiler evidence until a SRC1 equation
     * consumes it.  That equation needs the logical-device feature and is
     * deliberately bounded to the native witness shape. */
    key.src_color_blend_factor[0]=VK_BLEND_FACTOR_SRC1_COLOR;
    key.dst_color_blend_factor[0]=VK_BLEND_FACTOR_ZERO;
    key.color_blend_op[0]=VK_BLEND_OP_ADD;
    key.src_alpha_blend_factor[0]=VK_BLEND_FACTOR_ONE;
    key.dst_alpha_blend_factor[0]=VK_BLEND_FACTOR_ZERO;
    key.alpha_blend_op[0]=VK_BLEND_OP_ADD;
    runtime=NULL;
    assert(!ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==
        VK_ERROR_FEATURE_NOT_PRESENT && !runtime);
    key.feature_mask|=PS5VK_FEATURE_DUAL_SRC_BLEND;
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_SUCCESS && runtime);
    assert(((const struct ps5vk_runtime_graphics_program *)runtime)->dual_source_export==1u);
    ps5vk_runtime_graphics_free(NULL,runtime);
    struct ps5vk_compilation_cache *cache=
        ps5vk_compilation_cache_create(2,4u*1024u*1024u);
    assert(cache);
    runtime=NULL;
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&runtime)==VK_SUCCESS && runtime);
    assert(((const struct ps5vk_runtime_graphics_program *)runtime)->dual_source_export==1u);
    ps5vk_runtime_graphics_cached_release(cache,runtime);
    ps5vk_compilation_cache_destroy(cache);

    free((void *)key.fragment.words);
    key.fragment=read_module("build/runtime-graphics/triangle.frag.spv");
    compiled=(PsbcShaderOutput){0};found_format=found_mask=0;
    assert(psbc_compile_shader(key.fragment.words,key.fragment.word_count*4u,
        &options,&compiled)==PSBC_RESULT_OK);
    spi_format=shader_mask=0;
    for(unsigned i=0;i<compiled.metadata.context_register_count;++i) {
        const PsbcRegisterWrite *reg=&compiled.metadata.context_registers[i];
        if(reg->offset==PS5VK_TEST_SPI_SHADER_COL_FORMAT_OFFSET) {
            spi_format=reg->value;found_format=1;
        }
        if(reg->offset==PS5VK_TEST_CB_SHADER_MASK_OFFSET) {
            shader_mask=reg->value;found_mask=1;
        }
    }
    assert(spi_format==UINT32_C(4));
    assert(shader_mask==UINT32_C(15));
    assert(found_format && found_mask);
    assert(ps5vk_runtime_fragment_export(&compiled.metadata)==
        PS5VK_RUNTIME_FRAGMENT_EXPORT_SINGLE);
    psbc_free_output(&compiled);
    runtime=NULL;
    /* Enabling dualSrcBlend does not let an ordinary single-export fragment
     * shader satisfy a SRC1 equation. */
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==
        VK_ERROR_FEATURE_NOT_PRESENT && !runtime);
    key.src_color_blend_factor[0]=VK_BLEND_FACTOR_SRC_ALPHA;
    key.dst_color_blend_factor[0]=VK_BLEND_FACTOR_ONE;
    key.src_alpha_blend_factor[0]=VK_BLEND_FACTOR_SRC_ALPHA;
    key.dst_alpha_blend_factor[0]=VK_BLEND_FACTOR_ONE;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_SUCCESS && runtime);
    assert(!((const struct ps5vk_runtime_graphics_program *)runtime)->dual_source_export);
    ps5vk_runtime_graphics_free(NULL,runtime);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

/* The promoted contract.  The whole GFX1013 register contract is served - the
 * 98 applicable upstream blend.dual_source leaves drew every factor and
 * operation pair the family generates and passed in one 404-case run - and the
 * two doors that guard dual source stay shut: a SRC1 equation still needs the
 * feature enabled on this logical device and the compiler-proven secondary
 * export. */
static void check_dual_source_blend_contract(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/dual_source.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .blend_enable[0]=VK_TRUE,
        /* The upstream dual-source family draws random factor/operation
         * combinations on both channels; any encodable pair must be accepted. */
        .src_color_blend_factor[0]=VK_BLEND_FACTOR_DST_COLOR,
        .dst_color_blend_factor[0]=VK_BLEND_FACTOR_SRC1_ALPHA,
        .color_blend_op[0]=VK_BLEND_OP_SUBTRACT,
        .src_alpha_blend_factor[0]=VK_BLEND_FACTOR_CONSTANT_COLOR,
        .dst_alpha_blend_factor[0]=VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        .alpha_blend_op[0]=VK_BLEND_OP_MAX};
    const void *runtime=NULL;
    /* The equation alone never authorizes the draw: the device feature is the
     * first door and it is still shut without the enabled bit. */
    assert(!ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==
        VK_ERROR_FEATURE_NOT_PRESENT && !runtime);
    key.feature_mask|=PS5VK_FEATURE_DUAL_SRC_BLEND;
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_SUCCESS && runtime);
    assert(((const struct ps5vk_runtime_graphics_program *)runtime)->dual_source_export==1u);
    ps5vk_runtime_graphics_free(NULL,runtime);
    /* An ordinary fragment shader cannot satisfy a SRC1 equation merely because
     * the measurement build accepts the equation. */
    free((void *)key.fragment.words);
    key.fragment=read_module("build/runtime-graphics/triangle.frag.spv");
    runtime=NULL;
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==
        VK_ERROR_FEATURE_NOT_PRESENT && !runtime);
    /* A non-source1 equation takes the same widened contract on the plain
     * path, which is what the mixed quads of a dual-source leaf need. */
    key.src_color_blend_factor[0]=VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    key.dst_color_blend_factor[0]=VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    key.color_blend_op[0]=VK_BLEND_OP_REVERSE_SUBTRACT;
    key.src_alpha_blend_factor[0]=VK_BLEND_FACTOR_DST_ALPHA;
    key.dst_alpha_blend_factor[0]=VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    key.alpha_blend_op[0]=VK_BLEND_OP_MIN;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_SUCCESS && runtime);
    assert(!((const struct ps5vk_runtime_graphics_program *)runtime)->dual_source_export);
    ps5vk_runtime_graphics_free(NULL,runtime);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

/* A fragment module that declares two primary Output locations is the two-MRT
 * shape. The pinned compiler publishes the SAME 0x44/0xff pair it publishes for
 * dual source, so the registers alone cannot classify it and the interface
 * must; and this profile renders one colour attachment, so the pipeline stays
 * refused instead of silently dropping the second export. */
static void check_two_mrt_exports(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/two_mrt.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    assert(ps5vk_spirv_graphics_interface(&key));
    unsigned primary_mask=0; int secondary=0;
    assert(ps5vk_spirv_fragment_outputs(&key.fragment,&primary_mask,&secondary));
    assert(primary_mask==3u && !secondary);
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_FRAGMENT,
        .entrypoint="main",.optimise=true,.address32_hi=2,.rasterization_samples=1,
        /* Two targets, both the profile's colour format. */
        .spi_shader_col_format=0x44};
    PsbcShaderOutput compiled={0};
    assert(psbc_compile_shader(key.fragment.words,key.fragment.word_count*4u,
        &options,&compiled)==PSBC_RESULT_OK);
    uint32_t spi_format=0,shader_mask=0;
    for(unsigned i=0;i<compiled.metadata.context_register_count;++i) {
        const PsbcRegisterWrite *reg=&compiled.metadata.context_registers[i];
        if(reg->offset==PS5VK_TEST_SPI_SHADER_COL_FORMAT_OFFSET)spi_format=reg->value;
        if(reg->offset==PS5VK_TEST_CB_SHADER_MASK_OFFSET)shader_mask=reg->value;
    }
    assert(spi_format==UINT32_C(0x44) && shader_mask==UINT32_C(0xff));
    /* The register classification cannot tell this from dual source; the
     * interface does, and the adapter refuses the pipeline while the profile
     * serves one colour attachment. */
    assert(ps5vk_runtime_fragment_export(&compiled.metadata)==
        PS5VK_RUNTIME_FRAGMENT_EXPORT_DUAL);
    psbc_free_output(&compiled);
    const void *runtime=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)!=
        VK_SUCCESS && !runtime);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

/* The per-target write mask is CB_TARGET_MASK, one four-bit field per colour
 * target, so a target whose mask is zero is a shape this profile programmes:
 * the fragment export writes none of its channels and the render pass's own
 * load or clear is what puts pixels there. That is exactly how the pinned
 * render-pass module builds its attachment_write_mask leaves
 * (vktRenderPassTests.cpp:2912: start_index_1 zeroes the first target's mask
 * and leaves the second at fifteen), and it is the shape a two-target
 * pipeline has to reach the native path with. */
static void check_two_target_write_masks(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/two_mrt.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM},
        .color_attachment_count=2,
        .feature_mask=PS5VK_FEATURE_INDEPENDENT_BLEND,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={0,15}};
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *runtime=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_SUCCESS && runtime);
    const struct ps5vk_runtime_graphics_program *program=runtime;
    assert(program->fragment_shape==PS5VK_RUNTIME_FRAGMENT_SHAPE_TWO_MRT);
    ps5vk_runtime_graphics_free(NULL,runtime);

    /* The capability is still what authorises the shape, and a mask wider than
     * the register field stays refused. */
    runtime=NULL;
    key.feature_mask=0;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_ERROR_FEATURE_NOT_PRESENT && !runtime);
    key.feature_mask=PS5VK_FEATURE_INDEPENDENT_BLEND;
    key.color_write_mask[1]=0x10;
    assert(!ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_ERROR_FEATURE_NOT_PRESENT && !runtime);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

/* The shape the leaf that writes ONLY the second colour target builds: its
 * first target's write mask is zero, so the module's fragment stage declares
 * Location 1 alone and the pinned compiler publishes SPI_SHADER_COL_FORMAT=0x9
 * with CB_SHADER_MASK=0xf0 - the format nibble follows the module's single
 * output (declaration order), the mask names the attachment it targets. The
 * adapter admits it only for exactly that pipeline, and carries the shape so
 * the draw state can programme the coherent pair for the attachment that is
 * really written. */
static void check_second_target_only(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/second_mrt_only.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM},
        .color_attachment_count=2,
        .feature_mask=PS5VK_FEATURE_INDEPENDENT_BLEND,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={0,15}};
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *runtime=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&runtime)==VK_SUCCESS && runtime);
    const struct ps5vk_runtime_graphics_program *program=runtime;
    assert(program->fragment_shape==PS5VK_RUNTIME_FRAGMENT_SHAPE_SECOND_MRT);
    assert(!program->dual_source_export);
    /* The compiler's own pair for this module, and the classification of it. */
    assert(ps5vk_runtime_fragment_export(&program->fragment.metadata)==
        PS5VK_RUNTIME_FRAGMENT_EXPORT_SINGLE_SECOND);
    ps5vk_runtime_graphics_free(NULL,runtime);

    /* The shape belongs to the pipeline, not to the module: the same fragment
     * stage on a pipeline that writes the first target is torn (that target
     * would be written with no export), and the capability still authorises the
     * second target's own pipeline. */
    const void *refused=NULL;
    key.color_write_mask[0]=15;
    assert(!ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&refused)!=
        VK_SUCCESS && !refused);
    key.color_write_mask[0]=0;
    key.feature_mask=0;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&refused)!=
        VK_SUCCESS && !refused);
    key.feature_mask=PS5VK_FEATURE_INDEPENDENT_BLEND;
    key.color_attachment_count=1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&refused)!=
        VK_SUCCESS && !refused);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

/* ViewIndex is delivered to both stages through independently declared slots.
 * It is not a vertex attribute and does not admit other unsupported built-ins. */
/* Clip and cull distances leave the pre-raster stage through the packed
 * position registers, and the pinned compiler already emits the matching
 * context state. What this checks is the adapter contract: exactly the
 * consistent combinations are packaged, and a mask whose registers disagree
 * with it is refused instead of being trusted. GPU execution is a separate
 * native gate. */
static PsbcRegisterWrite *context_register(PsbcShaderMetadata *m,unsigned offset)
{
    for(unsigned i=0;i<m->context_register_count;++i)
        if(m->context_registers[i].offset==offset)return &m->context_registers[i];
    return NULL;
}

/* A DEPTH-ONLY pipeline: no colour attachment, so the key carries an undefined
 * colour format, writes no channel, and the fragment stage exports nothing.
 * Everything else is the ordinary triangle pair. */
static void check_depth_only_target(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/depth_only.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        /* No colour attachment at all: the count is zero and the format the
         * key carries for it is VK_FORMAT_UNDEFINED. */
        .color_format={VK_FORMAT_UNDEFINED},.color_attachment_count=0,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={0}};
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *out=NULL;
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *program=out;
    /* The pixel program really exports nothing: SPI_SHADER_COL_FORMAT and
     * CB_SHADER_MASK both read zero, which is the NONE export class. */
    PsbcShaderMetadata *m=(PsbcShaderMetadata *)&program->fragment.metadata;
    const PsbcRegisterWrite *format=context_register(m,0x1c5);
    const PsbcRegisterWrite *mask=context_register(m,0x08f);
    assert(format && !format->value);
    assert(mask && !mask->value);
    ps5vk_runtime_graphics_free(NULL,out);

    /* The three colour facts travel together. A colour format with no write
     * mask, or a write mask with no format, is not a depth-only pass and stays
     * refused; and an exporting fragment shader has nowhere to export. */
    out=NULL;
    key.color_write_mask[0]=15;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    key.color_write_mask[0]=0;
    free((void *)key.fragment.words);
    key.fragment=read_module("build/runtime-graphics/triangle.frag.spv");
    assert(!ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

static void check_clip_cull_distances(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/clip_distance.vert.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_SHADER_CLIP_DISTANCE};
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *out=NULL;
    /* The compiled pair is the usage evidence: without the feature enabled the
     * same key is refused even though the declaration policy accepted it. */
    key.feature_mask=0;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    key.feature_mask=PS5VK_FEATURE_SHADER_CLIP_DISTANCE;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p=out;
    assert(p->vertex.metadata.clip_distance_mask==0x03u);
    assert(!p->vertex.metadata.cull_distance_mask);
    /* The standalone information pass used to leave the whole linkage
     * unresolved because the packed distance register was not described; the
     * description now names it (key PSBC_SEMANTIC_DISTANCE_REGISTER + register,
     * parameter index above it), so the only unresolved fields left are the
     * program checksum and the link-time ring item size. */
    assert(p->vertex.metadata.unresolved_fields==
           (PSBC_UNRESOLVED_PROGRAM_CHECKSUM|PSBC_UNRESOLVED_NGG_ESGS_RING_ITEMSIZE));
    {
        unsigned described=0;
        for(uint32_t i=0;i<p->vertex.metadata.output_semantic_count;++i) {
            if((p->vertex.metadata.output_semantics[i]&255u)!=PSBC_SEMANTIC_DISTANCE_REGISTER)
                continue;
            assert(((p->vertex.metadata.output_semantics[i]>>8)&255u)==1u);
            ++described;
        }
        assert(described==1);
    }
    struct ps5vk_runtime_shader header;
    assert(!ps5vk_runtime_shader_build(&header,&p->vertex));
    /* State that contradicts the mask is refused: a non-contiguous clip mask,
     * a cull mask the registers do not carry, a missing packed position
     * register, a cleared distance enable, a changed export count, a width
     * past the two registers and a pixel stage that claims a distance. */
    PsbcShaderOutput mutated=p->vertex;
    mutated.metadata.clip_distance_mask=0x05u;
    assert(ps5vk_runtime_shader_build(&header,&mutated));
    mutated=p->vertex;
    mutated.metadata.cull_distance_mask=0x04u;
    assert(ps5vk_runtime_shader_build(&header,&mutated));
    mutated=p->vertex;
    context_register(&mutated.metadata,0x1c3u)->value=0x04u;
    assert(ps5vk_runtime_shader_build(&header,&mutated));
    mutated=p->vertex;
    context_register(&mutated.metadata,0x207u)->value&=~(1u<<22);
    assert(ps5vk_runtime_shader_build(&header,&mutated));
    mutated=p->vertex;
    context_register(&mutated.metadata,0x1b1u)->value=0u;
    assert(ps5vk_runtime_shader_build(&header,&mutated));
    mutated=p->vertex;
    mutated.metadata.clip_distance_mask=0xffu;
    mutated.metadata.cull_distance_mask=0x0fu;
    assert(ps5vk_runtime_shader_build(&header,&mutated));
    mutated=p->vertex;
    context_register(&mutated.metadata,0x1c3u)->offset=0x0fffu;
    assert(ps5vk_runtime_shader_build(&header,&mutated));
    mutated=p->fragment;
    mutated.metadata.clip_distance_mask=0x01u;
    assert(ps5vk_runtime_shader_build(&header,&mutated));
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);

    /* Cull-only and the four-plus-four ceiling: the two shapes whose packed
     * register count differs from the clip-only one. */
    key.vertex=read_module("build/runtime-graphics/cull_distance.vert.spv");
    key.fragment=read_module("build/runtime-graphics/triangle.frag.spv");
    key.feature_mask=PS5VK_FEATURE_SHADER_CULL_DISTANCE;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    p=out;
    assert(!p->vertex.metadata.clip_distance_mask && p->vertex.metadata.cull_distance_mask==0x01u);
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    key.vertex=read_module("build/runtime-graphics/clip_cull_distance.vert.spv");
    key.fragment=read_module("build/runtime-graphics/triangle.frag.spv");
    key.feature_mask=PS5VK_FEATURE_SHADER_CLIP_DISTANCE|PS5VK_FEATURE_SHADER_CULL_DISTANCE;
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    p=out;
    assert(p->vertex.metadata.clip_distance_mask==0x0fu &&
           p->vertex.metadata.cull_distance_mask==0xf0u);
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);

    /* The coverage witness declares both arrays and selects the distances with
     * one specialization constant, so every mode shares the packed mask pair
     * and the parameter accounting that counts the packed slots. The control is
     * the same vertex stage with neither array declared. */
    for(unsigned mode=0;mode<6;++mode) {
        struct ps5vk_graphics_key probe={
            .vertex=read_module("build/runtime-graphics/clip_cull_probe.vert.spv"),
            .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
            .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
            .feature_mask=PS5VK_FEATURE_SHADER_CLIP_DISTANCE|PS5VK_FEATURE_SHADER_CULL_DISTANCE};
        probe.vertex.specialization_count=1;
        probe.vertex.specializations[0]=(struct ps5vk_graphics_specialization){
            .constant_id=0,.size=sizeof(mode)};
        memcpy(probe.vertex.specializations[0].data,&mode,sizeof(mode));
        assert(ps5vk_spirv_graphics_interface(&probe));
        assert(ps5vk_runtime_graphics_compile(NULL,&probe,&out)==VK_SUCCESS && out);
        p=out;
        assert(p->vertex.metadata.clip_distance_mask==0x03u &&
               p->vertex.metadata.cull_distance_mask==0x0cu);
        ps5vk_runtime_graphics_free(NULL,out);
        free((void *)probe.vertex.words);free((void *)probe.fragment.words);
    }
    key.vertex=read_module("build/runtime-graphics/clip_cull_control.vert.spv");
    key.fragment=read_module("build/runtime-graphics/triangle.frag.spv");
    key.feature_mask=0;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    p=out;
    assert(!p->vertex.metadata.clip_distance_mask && !p->vertex.metadata.cull_distance_mask);
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    puts("Clip/cull distances: packed masks, register state and metadata refusal");
}

/* Rewrite every OpConstant whose value is `from`, which is how the per-vertex
 * array lengths of a geometry stage are declared (the built-in block and the
 * varying array share the input primitive's vertex count). */
static int patch_array_length(struct ps5vk_graphics_module_key *m,uint32_t from,uint32_t to)
{
    uint32_t *words=(uint32_t *)m->words;size_t patched=0;
    for(size_t at=5;at<m->word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;
        if((w[0]&65535u)==43u && (w[0]>>16)==4u && w[3]==from) { w[3]=to;++patched; }
    }
    return patched>=1;
}

/* The pixel end of the clip-distance interface. A fragment stage that READS
 * gl_ClipDistance is legal SPIR-V, and the description policy accepts it when
 * the pre-raster stage exports at least that many components. Delivery is a
 * separate fact about the compiled metadata: the distances travel in the packed
 * position registers the pre-raster stage exports, the compiler names those
 * registers on both sides (the producer's word carries the parameter index to
 * interpolate from), and the profile still refuses to RUN the pair until a
 * native witness shows the rasterizer delivers the interpolated value. This
 * checks the description against the real compiled metadata, the refusal in the
 * shipping profile, and the description predicate's own negatives.
 */
/* These compiler probes use a real set-0 combined image sampler declaration.
 * The implicit core gather, explicit component selectors, constant offset,
 * runtime single offset, four independent constant offsets, and depth-reference
 * gather each have to survive SPIR-V ingestion, PSBC compilation, runtime-header
 * validation and draw-ABI construction. The offset Dref form must be gated by
 * shaderImageGatherExtended just like color gathers. This is compiler evidence
 * only; pixel values still need the native gather oracle. */
static void check_gather_compiler_forms(void)
{
    const char *const fragments[]={
        "build/runtime-graphics/gather_core.frag.spv",
        "build/runtime-graphics/gather_const_offset.frag.spv",
        "build/runtime-graphics/gather_dynamic_offset.frag.spv",
        "build/runtime-graphics/gather_four_offsets.frag.spv",
        "build/runtime-graphics/gather_component_0.frag.spv",
        "build/runtime-graphics/gather_component_1.frag.spv",
        "build/runtime-graphics/gather_component_2.frag.spv",
        "build/runtime-graphics/gather_component_3.frag.spv",
        "build/runtime-graphics/gather_dref.frag.spv"};
    const int extended_required[]={0,1,1,1,0,0,0,0,1};
    struct ps5vk_set_signature set={0};
    set.binding[0]=(struct ps5vk_binding){
        .count=1,.stages=VK_SHADER_STAGE_FRAGMENT_BIT};
    set.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    set.count=1;
    /* Canonical signatures carry the running descriptor prefix through empty
     * bindings, exactly as vkCreateDescriptorSetLayout stores them. */
    for(unsigned b=1;b<PS5VK_MAX_BINDINGS;++b)set.binding[b].first=1;
    for(unsigned i=0;i<sizeof(fragments)/sizeof(fragments[0]);++i) {
        struct ps5vk_graphics_key key={
            .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
            .fragment=read_module(fragments[i]),
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .color_attachment_count=1,
            .color_format={VK_FORMAT_B8G8R8A8_UNORM},
            .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
            .descriptor_set_count=1,.descriptor_sets=&set};
        assert(ps5vk_spirv_graphics_interface(&key));
        const int extended=ps5vk_spirv_module_uses_extended_gather(&key.fragment);
        assert(extended==extended_required[i]);
        key.feature_mask=0;
        if(extended) {
            const void *refused=NULL;
            assert(!ps5vk_runtime_graphics_supported(&key));
            assert(ps5vk_runtime_graphics_compile(NULL,&key,&refused)==
                   VK_ERROR_FEATURE_NOT_PRESENT && !refused);
            key.feature_mask=PS5VK_GRAPHICS_FEATURE_IMAGE_GATHER_EXTENDED;
        }
        assert(ps5vk_runtime_graphics_supported(&key));
        const void *out=NULL;
        assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
        const struct ps5vk_runtime_graphics_program *program=out;
        assert(program->fragment.machine_code && program->fragment.machine_code_size);
        assert(program->fragment.metadata.descriptor_binding_count==1);
        assert(program->fragment.metadata.descriptor_set_valid[0]);
        assert(program->fragment.metadata.descriptor_used_binding_mask[0]==1u);
        struct ps5vk_runtime_shader header;
        assert(ps5vk_runtime_shader_build(&header,&program->fragment)==0);
        ps5vk_runtime_graphics_free(NULL,out);
        free((void *)key.vertex.words);free((void *)key.fragment.words);
    }
#if defined(PS5VK_RGBA8_INTEGER_ATTACHMENT_DIAGNOSTIC) && PS5VK_RGBA8_INTEGER_ATTACHMENT_DIAGNOSTIC
    const struct {
        const char *shader;
        VkFormat color_format;
        unsigned numeric;
    } integer_outputs[]={
        {"build/runtime-graphics/gather_uint.frag.spv",VK_FORMAT_R8G8B8A8_UINT,
         PS5VK_VERTEX_NUMERIC_UINT},
        {"build/runtime-graphics/gather_sint.frag.spv",VK_FORMAT_R8G8B8A8_SINT,
         PS5VK_VERTEX_NUMERIC_SINT},
    };
    for(unsigned i=0;i<sizeof(integer_outputs)/sizeof(integer_outputs[0]);++i) {
        struct ps5vk_graphics_key key={
            .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
            .fragment=read_module(integer_outputs[i].shader),
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .color_attachment_count=1,
            .color_format={integer_outputs[i].color_format},
            .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
            .descriptor_set_count=1,.descriptor_sets=&set,
            .feature_mask=PS5VK_GRAPHICS_FEATURE_IMAGE_GATHER_EXTENDED};
        assert(ps5vk_spirv_graphics_interface(&key));
        assert(ps5vk_runtime_graphics_supported(&key));
        const void *out=NULL;
        assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
        const struct ps5vk_runtime_graphics_program *program=out;
        assert(program->fragment.machine_code && program->fragment.machine_code_size);
        assert(program->fragment.metadata.descriptor_used_binding_mask[0]==1u);
        const PsbcRegisterWrite *spi=context_register(
            (PsbcShaderMetadata *)&program->fragment.metadata,0x1c5u);
        assert(spi);
        printf("integer gather host compiler: format=%u output_numeric=%u SPI_SHADER_COL_FORMAT=%u\n",
            (unsigned)integer_outputs[i].color_format,integer_outputs[i].numeric,
            spi->value & 0xfu);
        ps5vk_runtime_graphics_free(NULL,out);
        free((void *)key.vertex.words);free((void *)key.fragment.words);
    }
#endif
    puts("Image gather compiler forms: core selectors are baseline; gather offsets including depth-reference gather require the gated feature");
}

static void check_fragment_distance_read(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/clip_distance.vert.spv"),
        .fragment=read_module("build/runtime-graphics/clip_distance_read.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_SHADER_CLIP_DISTANCE};
    unsigned clip=~0u,cull=~0u;
    assert(ps5vk_spirv_stage_distance_reads(&key.fragment,&clip,&cull));
    assert(clip==2 && cull==0);
    assert(ps5vk_spirv_graphics_interface(&key));
    /* The interface decision is independent of delivery, so the profile keeps
     * accepting the shape; the run is what is refused. */
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *read_pair=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&read_pair)==VK_SUCCESS && read_pair);
    ps5vk_runtime_graphics_free(NULL,read_pair);
    /* The same pair without the read compiles, so the refusal above is the read
     * and not a side effect of the fixture. */
    struct ps5vk_graphics_module_key reads=key.fragment;
    key.fragment=read_module("build/runtime-graphics/triangle.frag.spv");
    const void *plain=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&plain)==VK_SUCCESS && plain);
    ps5vk_runtime_graphics_free(NULL,plain);
    free((void *)key.fragment.words);
    key.fragment=reads;
    assert(ps5vk_runtime_graphics_supported(&key));
    /* A read wider than the producer exports never reaches the pipeline gate:
     * the interface chain refuses it first, which is the bounded-declaration
     * half of the same contract. */
    struct ps5vk_graphics_module_key narrow=read_module("build/runtime-graphics/triangle.vert.spv");
    struct ps5vk_graphics_key too_wide=key;
    too_wide.vertex=narrow;
    assert(!ps5vk_spirv_graphics_interface(&too_wide));
    free((void *)narrow.words);
    /* The description predicate over the compiled pair: the producer names the
     * packed distance register (key 48, parameter index above it) and the pixel
     * stage names the same register as an input. Each negative changes exactly
     * one field of a consistent pair. */
    PsbcShaderMetadata pre={0},ps={0};
    pre.clip_distance_mask=0x3u;
    pre.output_semantic_count=2;
    pre.output_semantics[0]=0x0000000fu;                        /* colour varying, param 0 */
    pre.output_semantics[1]=PSBC_SEMANTIC_DISTANCE_REGISTER|(1u<<8);
    ps.ps_clip_distance_reads=2;
    ps.input_semantic_count=2;
    ps.input_semantics[0]=PSBC_SEMANTIC_DISTANCE_REGISTER;      /* distance register, attr 0 */
    ps.input_semantics[1]=0x0000000fu;                          /* colour varying, attr 1 */
    assert(ps5vk_runtime_graphics_distance_reads_described(&pre,&ps,2,0));
    assert(ps5vk_runtime_graphics_distance_reads_described(&pre,&ps,0,0));
    assert(!ps5vk_runtime_graphics_distance_reads_described(&pre,&ps,3,0)); /* reads > exports */
    PsbcShaderMetadata mutated=pre;
    mutated.output_semantics[1]=0x0000010fu;                    /* producer stopped naming it */
    assert(!ps5vk_runtime_graphics_distance_reads_described(&mutated,&ps,2,0));
    mutated=pre;mutated.clip_distance_mask=0x1u;                /* one component exported */
    assert(!ps5vk_runtime_graphics_distance_reads_described(&mutated,&ps,2,0));
    mutated=ps;mutated.ps_clip_distance_reads=1;                /* pixel report disagrees */
    assert(!ps5vk_runtime_graphics_distance_reads_described(&pre,&mutated,2,0));
    mutated=ps;mutated.input_semantics[0]=0x0000000fu;          /* pixel stopped naming it */
    assert(!ps5vk_runtime_graphics_distance_reads_described(&pre,&mutated,2,0));
    /* FS declares only cull; its packed slots follow the producer's three
     * clips, not a nonexistent consumer clip prefix. */
    pre=(PsbcShaderMetadata){.clip_distance_mask=7,.cull_distance_mask=0x78,
        .output_semantic_count=2,.output_semantics={0x130,0x231}};
    ps=(PsbcShaderMetadata){.ps_cull_distance_reads=4,.input_semantic_count=2,
        .input_semantics={0x30,0x31}};
    assert(ps5vk_runtime_graphics_distance_reads_described(&pre,&ps,0,4));
    mutated=pre;mutated.cull_distance_mask=0x38; /* fourth cull component absent */
    assert(!ps5vk_runtime_graphics_distance_reads_described(&mutated,&ps,0,4));
    mutated=pre;mutated.output_semantic_count=1;
    assert(!ps5vk_runtime_graphics_distance_reads_described(&mutated,&ps,0,4));
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

/* The optional geometry stage. The interface policy has to describe the whole
 * vertex -> geometry -> fragment link, and the compiler adapter must refuse a
 * geometry key until the merged pre-raster stage exists: compiling the vertex
 * stage alone and calling it a geometry pipeline would be the exact silent
 * substitution this profile refuses everywhere else. */
static void check_geometry_stage(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/geometry_probe.vert.spv"),
        .geometry=read_module("build/runtime-graphics/geometry_probe.geom.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER};
    const int32_t passthrough=0;
    key.geometry.specialization_count=1;
    key.geometry.specializations[0]=(struct ps5vk_graphics_specialization){
        .constant_id=0,.size=sizeof(passthrough)};
    memcpy(key.geometry.specializations[0].data,&passthrough,sizeof(passthrough));
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *out=(void *)1;
    /* The merged program is packaged by the shipping profile too. It used to be
     * refused because the ES->GS input handoff needed the link-time ring item
     * size, which the pinned compiler flags unresolved: executing it with a
     * guessed value corrupted the geometry the stage read. That is fixed and
     * witnessed - the hardware scales the per-vertex offsets by
     * VGT_ESGS_RING_ITEMSIZE, next-gen geometry keeps that at one, and the driver
     * now programs it that way - so the adapter accepts the pair, and what gates
     * the feature is the conformance selection, not this path. */
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    out=(void *)1;
    /* The same key without the feature is still refused: a geometry pipeline
     * needs the feature the logical device enabled, exactly like every other
     * stage. */
    key.feature_mask=0;
    assert(ps5vk_spirv_graphics_interface(&key));
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    /* A geometry stage whose per-vertex input array is not the three vertices of
     * one triangle has no input primitive this profile can feed it. */
    struct ps5vk_graphics_module_key patched=
        read_module("build/runtime-graphics/geometry_probe.geom.spv");
    assert(patch_array_length(&patched,3,2));
    struct ps5vk_graphics_key wrong=key;
    wrong.geometry=patched;
    assert(!ps5vk_spirv_graphics_interface(&wrong));
    assert(ps5vk_runtime_graphics_compile(NULL,&wrong,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)patched.words);
    /* The geometry invocation id. The feature's mandatory invocations minimum is
     * meaningless without it - an invocation that cannot tell which one it is can
     * only repeat the same work - so the policy has to describe a stage that
     * reads it, and this one is a geometry built-in rather than a vertex one. */
    struct ps5vk_graphics_key invoked=key;
    invoked.geometry=read_module("build/runtime-graphics/geometry_invocations.geom.spv");
    invoked.feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER;
    assert(ps5vk_spirv_graphics_interface(&invoked));
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&invoked,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)invoked.geometry.words);
    /* The input-primitive families. A device that advertises geometryShader is
     * expected to feed the stage points and lines as well as triangles, so the
     * declared per-vertex input array is bound to the pipeline's topology rather
     * than to one fixed length: each family declares its own arity (one vertex
     * per input point, two per input line) and packages with the primitive value
     * the linker will program. Patching the triangle module's array length is
     * NOT how this is tested - the module's own body reads three vertices, and
     * rewriting only the declaration produces SPIR-V the parser rejects (the
     * compiler traps on an OpCompositeConstruct whose operand count no longer
     * matches the rewritten array type) - so the families have their own
     * modules, which are the same ones the native witness uses. */
    const VkPrimitiveTopology families[3]={VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,VK_PRIMITIVE_TOPOLOGY_LINE_STRIP};
    const char *family_modules[3]={
        "build/runtime-graphics/geometry_points.geom.spv",
        "build/runtime-graphics/geometry_lines.geom.spv",
        "build/runtime-graphics/geometry_lines.geom.spv"};
    const uint32_t family_primitives[3]={PS5VK_AGC_PRIMITIVE_TYPE_POINT_LIST,
        PS5VK_AGC_PRIMITIVE_TYPE_LINE_LIST,PS5VK_AGC_PRIMITIVE_TYPE_LINE_STRIP};
    for(unsigned f=0;f<3;++f) {
        struct ps5vk_graphics_module_key family=read_module(family_modules[f]);
        struct ps5vk_graphics_key family_key=key;
        family_key.feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER;
        family_key.topology=families[f];
        family_key.geometry=family;
        assert(ps5vk_spirv_graphics_interface(&family_key));
        out=(void *)1;
        assert(ps5vk_runtime_graphics_compile(NULL,&family_key,&out)==VK_SUCCESS && out);
        assert(((const struct ps5vk_runtime_graphics_program *)out)->primitive_type==
            family_primitives[f]);
        ps5vk_runtime_graphics_free(NULL,out);
        free((void *)family.words);
    }
    /* The binding is a real constraint in both directions: the point module is
     * refused when the pipeline says triangles, and the line module when it says
     * points, so neither family can ride on another's primitive. */
    struct ps5vk_graphics_module_key point_module=
        read_module("build/runtime-graphics/geometry_points.geom.spv");
    struct ps5vk_graphics_key misbound=key;
    misbound.feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER;
    misbound.geometry=point_module;
    assert(!ps5vk_spirv_graphics_interface(&misbound));
    assert(ps5vk_runtime_graphics_compile(NULL,&misbound,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)point_module.words);
    struct ps5vk_graphics_module_key line_module=
        read_module("build/runtime-graphics/geometry_lines.geom.spv");
    misbound=key;
    misbound.feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER;
    misbound.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    misbound.geometry=line_module;
    assert(!ps5vk_spirv_graphics_interface(&misbound));
    assert(ps5vk_runtime_graphics_compile(NULL,&misbound,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)line_module.words);
    free((void *)key.vertex.words);free((void *)key.geometry.words);
    free((void *)key.fragment.words);
    puts("Geometry stage: vertex/geometry/fragment link described, merged package packaged for the point, line and triangle families");
}

/* multiViewport end to end. A geometry stage selects which of the pipeline's
 * viewport banks it writes to by writing gl_ViewportIndex, and core Vulkan lets
 * ONLY a geometry stage write it. The declaration is a pipeline shape this
 * profile can describe - refusing the shape outright would be stricter than
 * Vulkan - but it is also a capability: with a single bank programmed the index
 * would be silently ignored and every primitive would paint viewport zero, so
 * the adapter refuses the pipeline unless the logical device enabled
 * multiViewport. The refusal is asserted by its own diagnostic site, not by the
 * error code alone, so a failure for an unrelated reason cannot pass this test. */
static void check_viewport_index_routing(void)
{
    VkVertexInputBindingDescription binding={.binding=0,.stride=32,
        .inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2]={
        {.location=0,.binding=0,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.offset=0},
        {.location=1,.binding=0,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.offset=16}};
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/raster_witness.vert.spv"),
        .geometry=read_module("build/runtime-graphics/raster_viewport_index.geom.spv"),
        .fragment=read_module("build/runtime-graphics/vertex_format.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .vertex_binding_count=1,.vertex_bindings=&binding,
        .vertex_attribute_count=2,.vertex_attributes=attributes,
        .feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER};
    /* The built-in is described where it is legal and nowhere else: the geometry
     * module writes it, the vertex module of the same pipeline does not. */
    assert(ps5vk_spirv_stage_viewport_index(&key.geometry));
    assert(!ps5vk_spirv_stage_viewport_index(&key.vertex));
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    assert(ps5vk_runtime_graphics_diag_site==14);
    /* With the capability enabled the mask-driven gate stops refusing, and that
     * is as far as this profile can go today. MEASURED DEPENDENCY, named here
     * rather than hidden: the adapter then refuses the pair one step later, in
     * ps5vk_runtime_shader_build, because the pinned compiler leaves
     * PSBC_UNRESOLVED_AGC_LINKAGE set for a merged vertex+geometry program that
     * exports this built-in - its fill_output_semantics() accounts for the
     * described varyings, the primitive id and the distance registers but not
     * for the viewport-index vector it does program in PA_CL_VS_OUT_CNTL
     * (0x01280000, USE_VTX_VIEWPORT_INDX). The control is exact: the same
     * geometry module with only the "gl_ViewportIndex = gl_PrimitiveIDIn" line
     * removed compiles clean through this same key, so the export - not the
     * interface, the topology or the descriptor plan - is the discriminator.
     * That dependency is now fixed and pinned: the compiler names the export as
     * PSBC_SEMANTIC_VIEWPORT_INDEX (mpereiraesaa/opengnm-psbc#17, the pin in
     * tools/prepare_compiler_deps.py), so the same key compiles. This assertion
     * is what fails if the pin ever moves back to a compiler that does not name
     * it. Advertising the feature stays a separate question: it also needs an
     * applicable upstream CTS leaf, which UPSTREAM_CTS.md records as absent. */
    key.feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER|PS5VK_FEATURE_MULTI_VIEWPORT;
    assert(ps5vk_runtime_graphics_supported(&key));
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)key.vertex.words);free((void *)key.geometry.words);
    free((void *)key.fragment.words);
}

/* The output side of the component envelope. A stage may DECLARE sixty-four
 * output components and still have them dropped: only something that reads them
 * makes the declaration observable, so the pixel half of this case declares an
 * input for all sixteen vec4 locations the geometry half writes and folds the
 * whole set into the colour. This checks the pair against the real compiler
 * metadata - the pixel stage really declares those locations, and the adapter
 * packages the pair - and that a pixel stage reading a location the geometry
 * half does not write is refused instead of interpolating a register nothing
 * exports. */
static void check_geometry_output_components(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/geometry_components.vert.spv"),
        .geometry=read_module("build/runtime-graphics/geometry_components.geom.spv"),
        .fragment=read_module("build/runtime-graphics/geometry_output_components.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER};
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p=out;
    assert(p->primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST);
    /* The pixel half names the sixteen output locations plus the varying, so the
     * consumed set is in the compiled metadata rather than in the source only. */
    assert(p->fragment.metadata.input_semantic_count>=17u);
    ps5vk_runtime_graphics_free(NULL,out);
    /* The same pixel half against a geometry stage that writes only its colour:
     * the interface chain refuses the pair before the compiler is reached. */
    struct ps5vk_graphics_key narrow=key;
    narrow.geometry=read_module("build/runtime-graphics/geometry_probe.geom.spv");
    assert(!ps5vk_spirv_graphics_interface(&narrow));
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&narrow,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)narrow.geometry.words);
    free((void *)key.vertex.words);free((void *)key.geometry.words);
    free((void *)key.fragment.words);
    puts("Geometry output components: sixty-four written components consumed by the pixel half, and an unwritten read refused");
}

/* A descriptor the caller declared for the GEOMETRY stage. The pinned
 * conformance module binds its uniform buffer and its sampled image to that
 * stage - dEQP-VK.geometry.basic.output_vary_by_uniform and ..._by_texture - so
 * a layout whose binding names only the geometry stage has to be carried by the
 * merged pre-raster program rather than refused for not naming the vertex stage.
 * The same binding on a pipeline without a geometry stage is still refused,
 * because the stage projection would drop it and the draw would read a table the
 * caller never bound. */
/* The per-sample fetch stage the pinned multisample oracle compiles
 * (DXVK262-T06): a multisampled subpass input read whose sample index comes
 * from the uniform block, exactly as upstream declares it. Measured with the
 * real adapter and the pinned compiler: the interface accepts the module, the
 * descriptor table layout carries the two bindings the stage reads - the input
 * attachment at set 0 binding 0 and the uniform buffer at binding 1 - the
 * compiler compiles it, and the compiled metadata names both bindings as used.
 * An earlier hand probe of this stage refused, and that was the probe carrying
 * no descriptor signature at all: this case is what keeps that reading from
 * coming back as a "the compiler cannot do it" claim. */
static void check_subpass_fetch_compilation(void)
{
    struct ps5vk_set_signature sets[1]={0};
    sets[0].count=2;
    sets[0].binding[0].count=1;sets[0].binding[0].first=0;
    sets[0].binding[0].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    sets[0].type[0]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    sets[0].binding[1].count=1;sets[0].binding[1].first=1;
    sets[0].binding[1].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    sets[0].type[1]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    /* Empty slots keep the canonical prefix, exactly as above. */
    for(unsigned b=2;b<PS5VK_MAX_BINDINGS;++b)sets[0].binding[b].first=2;
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/runtime_subpass_fetch.frag.spv"),
        .descriptor_set_count=1,.descriptor_sets=sets,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_4_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_SAMPLE_RATE_SHADING};
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p=out;
    assert(p->fragment.machine_code_size);
    /* Both descriptors the stage names reach the compiled program: without the
     * input attachment the read would have no source, and without the uniform
     * buffer it would have no sample index. */
    assert(p->fragment.metadata.descriptor_set_valid[0]);
    assert(p->fragment.metadata.descriptor_used_binding_mask[0]==UINT64_C(0x3));
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    /* The same module without the layout's signature is refused, which is the
     * shape that produced the wrong reading: the refusal is the missing
     * declaration, not the shader. */
    struct ps5vk_graphics_key undeclared={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/runtime_subpass_fetch.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_4_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_SAMPLE_RATE_SHADING};
    assert(ps5vk_spirv_graphics_interface(&undeclared));
    const void *refused=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&undeclared,&refused)!=VK_SUCCESS && !refused);
    free((void *)undeclared.vertex.words);free((void *)undeclared.fragment.words);
}

/* The driver's OWN resolve stages (DXVK262-T06): one averaging fragment per
 * served sample count, paired with the oversized-triangle vertex stage and
 * compiled through the same runtime compiler the pipeline objects use. The
 * count the driver has no stage for stays refused rather than being resolved
 * with a stage that reads the wrong number of samples. */
static void check_resolve_program(void)
{
    for (unsigned index = 0; index < 2; ++index) {
        const VkSampleCountFlagBits samples = index ? VK_SAMPLE_COUNT_4_BIT :
            VK_SAMPLE_COUNT_2_BIT;
        struct ps5vk_resolve_program program = {0};
        assert(ps5vk_resolve_program_acquire(NULL, samples, &program) == VK_SUCCESS &&
               program.pair);
        const struct ps5vk_runtime_graphics_program *pair = program.pair;
        /* The averaging stage reads the input attachment and exports one colour
         * target: that is the whole shape a resolve draw needs. */
        assert(pair->fragment.machine_code_size);
        assert(pair->fragment.metadata.descriptor_set_valid[0]);
        assert(pair->fragment.metadata.descriptor_used_binding_mask[0] == UINT64_C(0x1));
        ps5vk_resolve_program_release(NULL, &program);
        assert(!program.pair);
    }
    /* 8x has no generated stage, and a count without a stage is refused rather
     * than averaged by a stage that reads the wrong samples. */
    struct ps5vk_resolve_program eight = {0};
    assert(ps5vk_resolve_program_acquire(NULL, VK_SAMPLE_COUNT_8_BIT, &eight) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !eight.pair);
    assert(ps5vk_resolve_program_acquire(NULL, VK_SAMPLE_COUNT_1_BIT, &eight) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !eight.pair);
}

static void check_geometry_stage_descriptor_visibility(void)
{
    struct ps5vk_set_signature sets[1]={0};
    sets[0].count=1;
    sets[0].binding[0].count=1;sets[0].binding[0].first=0;
    sets[0].binding[0].stages=VK_SHADER_STAGE_GEOMETRY_BIT;
    sets[0].type[0]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    /* Empty bindings keep the canonical prefix: the table layout compares every
     * slot against it, so a zeroed tail is a malformed signature, not an empty
     * one. */
    for(unsigned b=1;b<PS5VK_MAX_BINDINGS;++b)sets[0].binding[b].first=1;
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/geometry_uniform.vert.spv"),
        .geometry=read_module("build/runtime-graphics/geometry_uniform.geom.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .descriptor_set_count=1,.descriptor_sets=sets,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER};
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p=out;
    /* The merged pre-raster program is what reads the buffer, so the compiled
     * metadata has to name the binding and the draw ABI has to carry the table
     * for that stage: a pipeline that packaged without either would draw the
     * geometry half with no descriptor at all. */
    assert(p->vertex.metadata.descriptor_set_valid[0]);
    assert(p->arguments.vertex_descriptor_valid[0]);
    ps5vk_runtime_graphics_free(NULL,out);
    /* A shared layout may keep a binding for an absent geometry stage.
     * Neither remaining stage reads it or receives its descriptor table. */
    struct ps5vk_graphics_key without_geometry=key;
    without_geometry.geometry=(struct ps5vk_graphics_module_key){0};
    without_geometry.feature_mask=0;
    assert(ps5vk_runtime_graphics_supported(&without_geometry));
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&without_geometry,&out)==VK_SUCCESS && out);
    p=out;
    assert(!p->arguments.vertex_descriptor_valid[0]);
    assert(!p->arguments.fragment_descriptor_valid[0]);
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)key.vertex.words);free((void *)key.geometry.words);
    free((void *)key.fragment.words);
    puts("Geometry descriptors: merged-stage access is carried; unused absent-stage bindings need no table");
}

/* The tessellation pair: compiled through the hull and domain programs.
 *
 * The interface policy reads both stages, their execution modes and the
 * per-patch interface, and the program key carries the pair and the patch
 * control points. The pinned compiler links the hull (vertex half as the LS
 * program behind the control half's machine code) and publishes the evaluation
 * half as a loadable NGG package, so the adapter compiles the whole pipeline:
 * the hull and domain halves carry their own metadata, and the feature the
 * logical device enabled is checked against the compiled evidence. This host
 * test also exercises the pointer-free cache payload, independent stage maps
 * and retained leases. It does not establish native GPU execution; that needs
 * the separately identified own-shader and upstream CTS hardware runs. */
static void check_tessellation_stage(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/tess.vert.spv"),
        .tess_control=read_module("build/runtime-graphics/tess.tesc.spv"),
        .tess_eval=read_module("build/runtime-graphics/tess.tese.spv"),
        .fragment=read_module("build/runtime-graphics/tess.frag.spv"),
        .patch_control_points=3,
        .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_TESSELLATION_SHADER};
    assert(ps5vk_graphics_has_tessellation(&key));
    assert(ps5vk_graphics_tessellation_key_valid(&key));
    assert(ps5vk_spirv_graphics_interface(&key));
    struct ps5vk_graphics_key five=key;
    five.geometry=read_module("build/runtime-graphics/geometry_probe.geom.spv");
    five.feature_mask|=PS5VK_FEATURE_GEOMETRY_SHADER;
    assert(ps5vk_spirv_graphics_interface(&five));
    assert(ps5vk_runtime_graphics_supported(&five));
    const void *unsupported_five=(void *)(uintptr_t)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&five,&unsupported_five)==
        VK_SUCCESS && unsupported_five);
    const struct ps5vk_runtime_graphics_program *five_program=unsupported_five;
    assert(five_program->hull.machine_code && five_program->domain.machine_code &&
        five_program->domain.metadata.merged_es_source_stage==PSBC_STAGE_TESS_EVAL);
    ps5vk_runtime_graphics_free(NULL,unsupported_five);
    struct ps5vk_compilation_cache *five_cache=ps5vk_compilation_cache_create(8,16u*1024u*1024u);
    assert(five_cache);
    const void *five_cold=NULL,*five_warm=NULL,*without_geometry=NULL;
    assert(ps5vk_runtime_graphics_cached_acquire(five_cache,&five,&five_cold)==VK_SUCCESS);
    assert(ps5vk_runtime_graphics_cached_acquire(five_cache,&five,&five_warm)==VK_SUCCESS);
    assert(ps5vk_runtime_graphics_cached_acquire(five_cache,&key,&without_geometry)==VK_SUCCESS);
    const struct ps5vk_runtime_graphics_program *cold_five=five_cold,*warm_five=five_warm,
        *plain_tess=without_geometry;
    assert(cold_five->domain.machine_code==warm_five->domain.machine_code);
    assert(cold_five->domain.machine_code!=plain_tess->domain.machine_code);
    assert(warm_five->domain.metadata.merged_es_source_stage==PSBC_STAGE_TESS_EVAL);
    assert(warm_five->arguments.ring_table_valid);
    struct ps5vk_cache_stats five_stats;
    ps5vk_compilation_cache_get_stats(five_cache,&five_stats);
    assert(five_stats.compiles==2 && five_stats.hits==1);
    ps5vk_runtime_graphics_cached_release(five_cache,five_cold);
    assert(warm_five->domain.machine_code_size);
    ps5vk_runtime_graphics_cached_release(five_cache,five_warm);
    ps5vk_runtime_graphics_cached_release(five_cache,without_geometry);
    ps5vk_compilation_cache_destroy(five_cache);
    PsbcCompileOptions merged_options={.target=PSBC_TARGET_PS5,
        .stage=PSBC_STAGE_GEOMETRY,.entrypoint="main",.optimise=true,.ngg=true,
        .address32_hi=2,.patch_control_points=3,.rasterization_samples=1};
    PsbcShaderOutput merged_output={0};
    const PsbcResult merged_result=psbc_compile_tess_geometry_pipeline(
        key.tess_control.words,key.tess_control.word_count*4,
        key.tess_eval.words,key.tess_eval.word_count*4,
        five.geometry.words,five.geometry.word_count*4,&merged_options,&merged_output);
    fprintf(stderr,"TES_GS compiler result=%d bytes=%zu\n",merged_result,merged_output.machine_code_size);
    assert(merged_result==PSBC_RESULT_OK && merged_output.machine_code_size);
    assert(merged_output.metadata.ps5_ring_table_valid);
    assert(merged_output.metadata.merged_geometry &&
        merged_output.metadata.merged_es_source_stage==PSBC_STAGE_TESS_EVAL);
    /* Pinned R_028B54 ES_EN occupies bits3..4; ES_STAGE_DS is1. */
    assert(((merged_output.metadata.linkage_stages_en.value>>3)&3u)==1u);
    assert(merged_output.metadata.ps5_ring_table_user_data_dword+2<=
        merged_output.metadata.user_sgpr_count);
    assert(merged_output.metadata.user_data_window_base>0);
    assert(!merged_output.metadata.vertex_buffer_table_valid);
    assert(!(merged_output.metadata.unresolved_fields&PSBC_UNRESOLVED_TESS_PIPELINE));
    PsbcShaderMetadata unused_fragment={0};
    assert(!ps5vk_runtime_graphics_feature_use_ok(&merged_output.metadata,
        &unused_fragment,PS5VK_FEATURE_GEOMETRY_SHADER));
    assert(!ps5vk_runtime_graphics_feature_use_ok(&merged_output.metadata,
        &unused_fragment,PS5VK_FEATURE_TESSELLATION_SHADER));
    assert(ps5vk_runtime_graphics_feature_use_ok(&merged_output.metadata,
        &unused_fragment,PS5VK_FEATURE_GEOMETRY_SHADER|PS5VK_FEATURE_TESSELLATION_SHADER));
    struct ps5vk_runtime_shader unfinished_header;
    assert(ps5vk_runtime_shader_build(&unfinished_header,&merged_output)==0);
    psbc_free_output(&merged_output);
    free((void *)five.geometry.words);
    /* The whole pipeline compiles: hull, domain and fragment. */
    assert(ps5vk_runtime_graphics_supported(&key));
    const void *out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p=out;
    /* The hull: ONE merged LS/HS program, launchable, so no tessellation
     * bit and no separate LS carriage. The launch state the driver owns
     * (stage enables, LS_HS_CONFIG, the rings, LDS_SIZE) is still the
     * driver's and is not in the package. */
    assert(p->hull.machine_code && p->hull.machine_code_size);
    assert(p->hull.metadata.source_stage==PSBC_STAGE_TESS_CTRL);
    assert(p->hull.metadata.hardware_stage==PSBC_HW_STAGE_HULL);
    assert(!(p->hull.metadata.unresolved_fields & PSBC_UNRESOLVED_TESS_PIPELINE));
    assert(!p->hull.metadata.hull_ls_valid);
    assert(p->hull.metadata.hull_tess_wg_valid);
    /* The domain half: the loadable NGG package, no tessellation bit. */
    assert(p->domain.machine_code && p->domain.machine_code_size);
    assert(p->domain.metadata.source_stage==PSBC_STAGE_TESS_EVAL);
    assert(p->domain.metadata.hardware_stage==PSBC_HW_STAGE_NGG);
    assert(p->domain.metadata.unresolved_fields==
        (PSBC_UNRESOLVED_PROGRAM_CHECKSUM |
         PSBC_UNRESOLVED_NGG_ESGS_RING_ITEMSIZE));
    /* No pre-raster vertex compile: the hull consumed the vertex half. */
    assert(!p->vertex.machine_code && !p->vertex.metadata.source_stage);
    assert(p->fragment.machine_code && p->fragment.machine_code_size);
    assert(p->primitive_type==9); /* DI_PT_PATCH, pinned gfx103 register data */
    ps5vk_runtime_graphics_free(NULL,out);

    /* The feature is checked against the compiled evidence: the same pipeline
     * on a device that did not enable tessellationShader is refused. */
    struct ps5vk_graphics_key disabled=key;
    disabled.feature_mask=0;
    assert(ps5vk_runtime_graphics_supported(&disabled));
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&disabled,&out)==
        VK_ERROR_FEATURE_NOT_PRESENT && !out);

    struct ps5vk_compilation_cache *tess_cache=ps5vk_compilation_cache_create(8,4*1024*1024);
    assert(tess_cache);
    const void *cold=NULL,*warm=NULL;
    assert(ps5vk_runtime_graphics_cached_acquire(tess_cache,&key,&cold)==VK_SUCCESS);
    assert(ps5vk_runtime_graphics_cached_acquire(tess_cache,&key,&warm)==VK_SUCCESS);
    const struct ps5vk_runtime_graphics_program *cp=cold,*wp=warm;
    assert(cp->hull.machine_code==wp->hull.machine_code &&
           cp->domain.machine_code==wp->domain.machine_code &&
           cp->fragment.machine_code==wp->fragment.machine_code);
    assert(!cp->vertex.machine_code && cp->patch_control_points==3 && cp->tess_output_points==3);
    struct ps5vk_cache_stats tess_stats;
    ps5vk_compilation_cache_get_stats(tess_cache,&tess_stats);
    assert(tess_stats.hits==1 && tess_stats.compiles==1 && tess_stats.current_entries==1);
    /* A truncated payload must be rejected without leaking the hit reference. */
    struct ps5vk_cache_entry *cached_entry=tess_cache->lru_head;
    const size_t saved_payload_bytes=cached_entry->payload_bytes;
    const int saved_refs=cached_entry->refcount;
    cached_entry->payload_bytes=1;
    out=(void *)1;
    assert(ps5vk_runtime_graphics_cached_acquire(tess_cache,&key,&out)==VK_ERROR_UNKNOWN && !out);
    assert(cached_entry->refcount==saved_refs);
    cached_entry->payload_bytes=saved_payload_bytes;
    /* Each individual stage map changes identity even if its ID is unused. */
    for(unsigned stage=0;stage<3;++stage) {
        struct ps5vk_graphics_key changed=key;
        struct ps5vk_graphics_module_key *m=stage==0?&changed.vertex:
            stage==1?&changed.tess_control:&changed.tess_eval;
        m->specialization_count=1;
        m->specializations[0]=(struct ps5vk_graphics_specialization){.constant_id=37,.size=4};
        out=NULL;
        assert(ps5vk_runtime_graphics_cached_acquire(tess_cache,&changed,&out)==VK_SUCCESS);
        ps5vk_runtime_graphics_cached_release(tess_cache,out);
    }
    ps5vk_compilation_cache_get_stats(tess_cache,&tess_stats);
    assert(tess_stats.compiles==4 && tess_stats.current_entries==4 && tess_stats.hits==2);
    const uint32_t code_word=*(const uint32_t *)cp->hull.machine_code;
    ps5vk_compilation_cache_destroy(tess_cache);
    assert(*(const uint32_t *)cp->hull.machine_code==code_word);
    ps5vk_runtime_graphics_cached_release(NULL,cold);
    assert(*(const uint32_t *)wp->hull.machine_code==code_word);
    ps5vk_runtime_graphics_cached_release(NULL,warm);
    /* Preserve the existing cache contract: over-budget insertion fails
     * closed without keeping an unaccounted fallback allocation. */
    tess_cache=ps5vk_compilation_cache_create(1,1);
    assert(tess_cache);
    out=NULL;
    assert(ps5vk_runtime_graphics_cached_acquire(tess_cache,&key,&out)==VK_ERROR_OUT_OF_HOST_MEMORY && !out);
    ps5vk_compilation_cache_get_stats(tess_cache,&tess_stats);
    assert(!tess_stats.current_entries && !tess_stats.current_bytes);
    ps5vk_compilation_cache_destroy(tess_cache);

    /* Input assembly and TCS output sizes are independent and must survive
     * compilation separately for the native launch registers. */
    struct ps5vk_graphics_key wrong=key;
    wrong.patch_control_points=4;
    assert(ps5vk_spirv_graphics_interface(&wrong));
    assert(ps5vk_runtime_graphics_supported(&wrong));
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&wrong,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *asymmetric=out;
    assert(asymmetric->patch_control_points==4 && asymmetric->tess_output_points==3);
    ps5vk_runtime_graphics_free(NULL,out);

    /* Half a pair is refused even before the stages are read. */
    struct ps5vk_graphics_key half=key;
    half.tess_eval=(struct ps5vk_graphics_module_key){0};
    assert(!ps5vk_graphics_tessellation_key_valid(&half));
    assert(!ps5vk_runtime_graphics_supported(&half));

    /* The same vertex and fragment modules without the pair are a supported
     * pipeline: the tessellation path above is about the pair, not about
     * the modules themselves. */
    struct ps5vk_graphics_key plain=key;
    plain.tess_control=(struct ps5vk_graphics_module_key){0};
    plain.tess_eval=(struct ps5vk_graphics_module_key){0};
    plain.patch_control_points=0;
    plain.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    assert(!ps5vk_graphics_has_tessellation(&plain));
    assert(ps5vk_spirv_graphics_interface(&plain));
    out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&plain,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);

    /* Unused specialization IDs are legal, independently on every source.
     * Actual same-ID resource-selection semantics are checked by the PSBC
     * tess_resource_metadata fixture, not inferred from this acceptance. */
    struct ps5vk_graphics_key specialized=key;
    struct ps5vk_graphics_module_key *spec_stages[]={
        &specialized.vertex,&specialized.tess_control,&specialized.tess_eval};
    for(unsigned s=0;s<3;++s) {
        spec_stages[s]->specialization_count=1;
        spec_stages[s]->specializations[0]=(struct ps5vk_graphics_specialization){
            .constant_id=37,.size=4};
        memcpy(spec_stages[s]->specializations[0].data,&s,4);
    }
    out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&specialized,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    specialized.tess_control.specialization_count=65;
    out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&specialized,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);

    /* Each source retains its own entrypoint through both merged and link-only
     * compiles. Rename only the four-byte SPIR-V entry string, preserving IDs
     * and executable instructions; all names deliberately differ. */
    key.geometry=read_module("build/runtime-graphics/geometry_probe.geom.spv");
    const char *entries[]={"vert","hull","eval","frag","geom"};
    struct ps5vk_graphics_module_key *entry_stages[]={
        &key.vertex,&key.tess_control,&key.tess_eval,&key.fragment,&key.geometry};
    for(unsigned s=0;s<5;++s) {
        uint32_t *words=(uint32_t *)entry_stages[s]->words;
        unsigned renamed=0;
        for(size_t at=5;at<entry_stages[s]->word_count;at+=words[at]>>16) {
            if((words[at]&65535u)==15u) {
                assert((words[at]>>16)>=5 && !memcmp(words+at+3,"main",5));
                memcpy(words+at+3,entries[s],4);++renamed;
            }
        }
        assert(renamed==1);entry_stages[s]->entry=entries[s];
    }
    key.feature_mask|=PS5VK_FEATURE_GEOMETRY_SHADER;
    out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    for(unsigned s=0;s<5;++s) {
        entry_stages[s]->entry="none";out=(void *)(uintptr_t)1;
        assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
        entry_stages[s]->entry=entries[s];
    }
    /* Preserve the standalone domain path and its missing-entry regressions. */
    free((void *)key.geometry.words);
    key.geometry=(struct ps5vk_graphics_module_key){0};
    key.feature_mask&=~PS5VK_FEATURE_GEOMETRY_SHADER;
    out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    for(unsigned s=0;s<4;++s) {
        entry_stages[s]->entry="none";out=(void *)(uintptr_t)1;
        assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
        entry_stages[s]->entry=entries[s];
    }

    free((void *)key.vertex.words);free((void *)key.tess_control.words);
    free((void *)key.tess_eval.words);free((void *)key.fragment.words);
    puts("Tessellation stage: hull and domain compiled, feature and cache fail closed");
}

static void check_view_index_builtin(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/view_index.vert.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    /* The vertex stage reads the built-in and NO vertex attribute, so the
     * interface must accept it with an empty input map. */
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    const struct ps5vk_runtime_graphics_program *p=compiled;
    const PsbcShaderMetadata *vs=&p->vertex.metadata;
    /* Metadata v14 declares the slot, inside the user-SGPR block the stage
     * really uses, and the fragment stage that does not read the built-in must
     * not declare one. */
    assert(vs->view_index_valid);
    assert(vs->view_index_user_data_dword<vs->user_sgpr_count);
    assert(!p->fragment.metadata.view_index_valid);
    assert(p->arguments.view_index_slot==vs->view_index_user_data_dword);
    assert(p->arguments.view_index_slot!=UINT32_MAX &&
           p->arguments.view_index_slot<p->arguments.vertex_count);
    /* And the ABI really carries the view: the declared slot holds the index the
     * caller passed, beside the other draw parameters. */
    uint32_t vertex[16],pixel[16];
    const uint32_t tables[4]={0,0,0,0};
    assert(!ps5vk_runtime_draw_values_sets(&p->arguments,7u,0u,0u,5u,0u,0u,tables,vertex,pixel));
    assert(vertex[p->arguments.view_index_slot]==5u);
    assert(vertex[p->arguments.base_vertex_slot]==7u);
    ps5vk_runtime_graphics_free(NULL,compiled);
    struct ps5vk_graphics_key both=key;
    both.fragment=read_module("build/runtime-graphics/view_index.frag.spv");
    assert(ps5vk_spirv_graphics_interface(&both));
    compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&both,&compiled)==VK_SUCCESS && compiled);
    p=compiled;
    assert(p->fragment.metadata.view_index_valid);
    assert(p->arguments.fragment_view_index_valid);
    assert(p->arguments.fragment_view_index_slot==p->fragment.metadata.view_index_user_data_dword);
    for(uint32_t view=0;view<6;++view) {
        assert(!ps5vk_runtime_draw_values_sets(&p->arguments,7,0,0,view,0,0,tables,vertex,pixel));
        assert(vertex[p->arguments.view_index_slot]==view);
        assert(pixel[p->arguments.fragment_view_index_slot]==view);
    }
    ps5vk_runtime_graphics_free(NULL,compiled);
    free((void *)both.fragment.words);
    /* Every refusal below leaves the output untouched (null), so the caller can
     * never mistake a failed compilation for a program to free. */
    const void *out=NULL;
    /* ViewIndex is a vertex input built-in, never an attribute. A key that ALSO
     * declares an attribute at a location no shader input reads is legal Vulkan
     * - the attribute is dropped rather than matched, and the fetch path prepares
     * only the spans the compiled input reads - so the interface accepts it. The
     * pinned geometry module's primitive_id_in leaf is exactly that shape. */
    VkVertexInputBindingDescription binding={.binding=0,.stride=4,
        .inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute={.location=0,.binding=0,
        .format=VK_FORMAT_R32_SINT,.offset=0};
    struct ps5vk_graphics_key attributed=key;
    attributed.vertex_binding_count=1;attributed.vertex_attribute_count=1;
    attributed.vertex_bindings=&binding;attributed.vertex_attributes=&attribute;
    assert(ps5vk_spirv_graphics_interface(&attributed));
    const void *attributed_out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&attributed,&attributed_out)==VK_SUCCESS &&
        attributed_out);
    ps5vk_runtime_graphics_free(NULL,attributed_out);
    /* The reverse stays refused: a shader input with no attribute to feed it. */
    struct ps5vk_graphics_key unfed=key;
    unfed.vertex=read_module("build/runtime-graphics/vertex_input.vert.spv");
    unfed.vertex_binding_count=0;unfed.vertex_attribute_count=0;
    unfed.vertex_bindings=NULL;unfed.vertex_attributes=NULL;
    assert(!ps5vk_spirv_graphics_interface(&unfed));
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&unfed,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)unfed.vertex.words);

    /* Changing only the execution model does not turn this vertex module into
     * a legal fragment module: it still has VertexIndex and a position block. */
    struct ps5vk_graphics_module_key fragment_model=read_module("build/runtime-graphics/view_index.vert.spv");
    assert(patch_entry_model(&fragment_model,4u));
    struct ps5vk_graphics_key wrong_stage=key;
    wrong_stage.fragment=fragment_model;
    assert(!ps5vk_spirv_graphics_interface(&wrong_stage));
    assert(ps5vk_runtime_graphics_compile(NULL,&wrong_stage,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)fragment_model.words);

    /* And an unknown built-in stays refused: only the five draw parameters and
     * ViewIndex are delivered this way. */
    struct ps5vk_graphics_module_key unknown=read_module("build/runtime-graphics/view_index.vert.spv");
    assert(patch_builtin(&unknown,4440u,4441u));
    struct ps5vk_graphics_key unknown_key=key;
    unknown_key.vertex=unknown;
    assert(!ps5vk_spirv_graphics_interface(&unknown_key));
    assert(ps5vk_runtime_graphics_compile(NULL,&unknown_key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)unknown.words);
    free((void *)key.vertex.words);
    free((void *)key.fragment.words);
}
/* A four-set layout whose fragment shader dereferences one sampler and whose
 * vertex shader dereferences none: the declaration survives in the metadata,
 * but only the set the optimized NIR really reads becomes a native requirement.
 * This is the contract that stops an unused layout set from being demanded. */
static void check_sparse_layout_static_use(void)
{
    struct ps5vk_set_signature sets[PS5VK_MAX_SETS]={0};
    for(unsigned s=0;s<PS5VK_MAX_SETS;++s) {
        sets[s].count=1;
        sets[s].binding[0].count=1;sets[s].binding[0].first=0;
        sets[s].binding[0].stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
        sets[s].type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        for(unsigned b=1;b<PS5VK_MAX_BINDINGS;++b)sets[s].binding[b].first=1;
    }
    VkVertexInputBindingDescription binding={.binding=0,.stride=24,
        .inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2]={
        {.location=0,.binding=0,.format=VK_FORMAT_R32G32B32_SFLOAT,.offset=0},
        {.location=1,.binding=0,.format=VK_FORMAT_R32G32B32_SFLOAT,.offset=12}};
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/mipmap.vert.spv"),
        .fragment=read_module("build/runtime-graphics/texture.frag.spv"),
        .descriptor_set_count=PS5VK_MAX_SETS,.descriptor_sets=sets,
        .vertex_binding_count=1,.vertex_attribute_count=2,
        .vertex_bindings=&binding,.vertex_attributes=attributes,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    const void *out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p=out;
    assert(p->primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST);
    assert(p->fragment.metadata.descriptor_binding_count==PS5VK_MAX_SETS);
    assert(p->fragment.metadata.descriptor_set_valid[0]);
    for(unsigned s=1;s<PS5VK_MAX_SETS;++s) {
        assert(!p->fragment.metadata.descriptor_set_valid[s]);
        assert(!p->arguments.fragment_descriptor_valid[s]);
        assert(!p->arguments.vertex_descriptor_valid[s]);
    }
    assert(p->arguments.fragment_descriptor_valid[0] &&
           !p->arguments.vertex_descriptor_valid[0]);
    /* Only the used set may require a table; the other three stay empty. */
    const uint32_t tables[PS5VK_MAX_SETS]={UINT32_C(0x1000),0,0,0};
    uint32_t vertex[16],pixel[16];
    assert(!ps5vk_runtime_draw_values_sets(&p->arguments,0,0,0,0,
        p->arguments.vertex_buffer_valid?16u:0u,0,tables,vertex,pixel));
    assert(pixel[p->arguments.fragment_descriptor_slot[0]]==UINT32_C(0x1000));
    ps5vk_runtime_graphics_free(NULL,out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

/* The original sampled-cube-array shader requires SampledCubeArray. Both
 * that capability and ImageCubeArray are gated by the logical device feature. */
static void check_cube_array_feature_mask(void)
{
    struct ps5vk_set_signature sampled={0};
    sampled.count=1;sampled.binding[0].count=1;sampled.binding[0].first=0;
    sampled.binding[0].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    sampled.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for(unsigned b=1;b<PS5VK_MAX_BINDINGS;++b)sampled.binding[b].first=1;
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/cube_array.frag.spv"),
        .descriptor_set_count=1,.descriptor_sets=&sampled,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    uint32_t *fragment_words=(uint32_t *)key.fragment.words;
    unsigned sampled_cube_array=0;
    for(size_t at=5;at<key.fragment.word_count;) {
        unsigned count=fragment_words[at]>>16;
        assert(count && count<=key.fragment.word_count-at);
        if((fragment_words[at]&65535u)==17u && count==2u &&
           fragment_words[at+1]==45u)sampled_cube_array=1;
        at+=count;
    }
    assert(sampled_cube_array);

    const void *out=NULL;
    assert(!ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==
        VK_ERROR_FEATURE_NOT_PRESENT && !out);

    key.feature_mask|=PS5VK_FEATURE_IMAGE_CUBE_ARRAY;
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *program=out;
    assert(program->fragment.metadata.descriptor_set_valid[0]);
    assert(program->arguments.fragment_descriptor_valid[0]);
    assert(program->arguments.fragment_used_bindings[0]&UINT64_C(1));
    ps5vk_runtime_graphics_free(NULL,out);

    /* Independently check ImageCubeArray capability 34 before PSBC parsing. */
    uint32_t *image_cube_words=malloc(key.fragment.word_count*sizeof(*image_cube_words));
    assert(image_cube_words);
    memcpy(image_cube_words,key.fragment.words,key.fragment.word_count*sizeof(*image_cube_words));
    int patched=0;
    for(size_t at=5;at<key.fragment.word_count;) {
        unsigned count=image_cube_words[at]>>16;
        assert(count && count<=key.fragment.word_count-at);
        if((image_cube_words[at]&65535u)==17u && count==2u &&
           image_cube_words[at+1]==45u) {
            image_cube_words[at+1]=34u;patched=1;break;
        }
        at+=count;
    }
    assert(patched);
    struct ps5vk_graphics_key image_cube_key=key;
    image_cube_key.fragment.words=image_cube_words;
    image_cube_key.feature_mask=0;
    assert(!ps5vk_runtime_graphics_supported(&image_cube_key));
    free(image_cube_words);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

static void check_descriptor_options(void)
{
    struct ps5vk_set_signature sets[4]={0};
    for(unsigned s=0;s<4;++s) {
        sets[s].binding[2].count=2;
        sets[s].binding[2].stages=VK_SHADER_STAGE_VERTEX_BIT;
        sets[s].type[2]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sets[s].binding[7].count=24;
        sets[s].binding[7].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
        sets[s].type[7]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            sets[s].binding[b].first=sets[s].count;
            sets[s].count+=sets[s].binding[b].count;
        }
    }
    struct ps5vk_graphics_key key={.descriptor_set_count=4,.descriptor_sets=sets};
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_FRAGMENT,
        .entrypoint="main",.optimise=true,.address32_hi=2,.primitive_type=4,.rasterization_samples=1};
    assert(ps5vk_runtime_graphics_descriptor_options(&key,VK_SHADER_STAGE_FRAGMENT_BIT,&options)==VK_SUCCESS);
    assert(options.descriptor_binding_count==4);
    for(unsigned s=0;s<4;++s) {
        const PsbcDescriptorBinding *b=&options.descriptor_bindings[s];
        assert(b->set==s && b->binding==7 && b->array_size==24 && b->offset==32 && b->stride==48);
        assert(b->type==PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER);
    }
    struct ps5vk_graphics_module_key module=read_module("build/runtime-graphics/descriptor_arrays.frag.spv");
    PsbcShaderOutput output={0};
    assert(psbc_compile_shader(module.words,module.word_count*4,&options,&output)==PSBC_RESULT_OK);
    assert(output.machine_code_size && output.metadata.hardware_stage==PSBC_HW_STAGE_PIXEL);
    assert(output.metadata.descriptor_binding_count==4);
    for(unsigned s=0;s<4;++s) {
        assert(output.metadata.descriptor_set_valid[s]);
        assert(output.metadata.descriptor_set_user_data_dword[s]<output.metadata.user_sgpr_count);
        const PsbcDescriptorBinding *b=&output.metadata.descriptor_bindings[s];
        assert(b->set==s && b->binding==7 && b->array_size==24 && b->offset==32 && b->stride==48);
    }
    struct ps5vk_runtime_shader header;
    assert(!ps5vk_runtime_shader_build(&header,&output));
    struct ps5vk_graphics_key base={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    const void *compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&base,&compiled)==VK_SUCCESS);
    const struct ps5vk_runtime_graphics_program *program=compiled;
    struct ps5vk_runtime_draw_abi abi;
    assert(!ps5vk_runtime_draw_abi_build(&program->vertex.metadata,&output.metadata,&abi));
    const uint32_t tables[4]={0x1000,0x2000,0x3000,0x4000};
    uint32_t vs[16],fs[16];
    assert(!ps5vk_runtime_draw_values_sets(&abi,0,0,0,0,0,0,tables,vs,fs));
    for(unsigned s=0;s<4;++s)assert(fs[abi.fragment_descriptor_slot[s]]==tables[s]);
    assert(ps5vk_runtime_draw_values(&abi,0,0,0,0,0,tables[0],vs,fs));
    ps5vk_runtime_graphics_free(NULL,compiled);
    free((void *)base.vertex.words);free((void *)base.fragment.words);
    psbc_free_output(&output);free((void *)module.words);
    /* Buffer resources are not yet in the enabled graphics profile. */
    assert(!ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_descriptor_options(&key,VK_SHADER_STAGE_VERTEX_BIT,&options)==VK_SUCCESS);
    assert(options.descriptor_binding_count==4);
    for(unsigned s=0;s<4;++s) {
        const PsbcDescriptorBinding *b=&options.descriptor_bindings[s];
        assert(b->set==s && b->binding==2 && b->array_size==2 && !b->offset && b->stride==16);
    }
    PsbcCompileOptions saved=options;
    sets[3].binding[8].first--;
    assert(ps5vk_runtime_graphics_descriptor_options(&key,VK_SHADER_STAGE_VERTEX_BIT,&options)!=VK_SUCCESS);
    assert(!memcmp(&saved,&options,sizeof(options)));sets[3].binding[8].first++;
    assert(ps5vk_runtime_graphics_descriptor_options(&key,VK_SHADER_STAGE_COMPUTE_BIT,&options)!=VK_SUCCESS);
    assert(!memcmp(&saved,&options,sizeof(options)));
    memset(sets,0,sizeof(sets));
    for(unsigned s=0;s<4;++s)for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
        sets[s].binding[b]=(struct ps5vk_binding){.first=b,.count=1,.stages=VK_SHADER_STAGE_FRAGMENT_BIT};
        sets[s].type[b]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;++sets[s].count;
    }
    assert(ps5vk_runtime_graphics_descriptor_options(&key,VK_SHADER_STAGE_FRAGMENT_BIT,&options)==VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!memcmp(&saved,&options,sizeof(options)));
    memset(sets,0,sizeof(sets));
    for(unsigned s=0;s<4;++s) {
        sets[s].count=24;sets[s].binding[7]=(struct ps5vk_binding){24,0,VK_SHADER_STAGE_FRAGMENT_BIT};
        sets[s].type[7]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        for(unsigned b=8;b<PS5VK_MAX_BINDINGS;++b)sets[s].binding[b].first=24;
    }
    key.vertex=read_module("build/runtime-graphics/triangle.vert.spv");
    key.fragment=read_module("build/runtime-graphics/descriptor_arrays.frag.spv");
    key.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;key.color_format[0]=VK_FORMAT_B8G8R8A8_UNORM;
    key.color_attachment_count=1;
    key.samples=VK_SAMPLE_COUNT_1_BIT;key.color_write_mask[0]=15;
    struct ps5vk_compilation_cache *cache=ps5vk_compilation_cache_create(4,1024*1024);
    assert(cache);
    const void *cold,*warm;
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&cold)==VK_SUCCESS);
    /* The lease view carries the primitive the cached pair was compiled for;
     * the payload check refuses a value that does not match the key topology. */
    assert(((const struct ps5vk_runtime_graphics_program *)cold)->primitive_type==
        PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST);
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&warm)==VK_SUCCESS);
    const struct ps5vk_runtime_graphics_program *actual=warm;
    for(unsigned s=0;s<4;++s) {
        assert(actual->arguments.fragment_descriptor_valid[s]);
        assert(actual->fragment.metadata.descriptor_bindings[s].array_size==24);
        assert(actual->fragment.metadata.descriptor_bindings[s].offset==0);
    }
    struct ps5vk_cache_stats stats;ps5vk_compilation_cache_get_stats(cache,&stats);
    assert(stats.compiles==1 && stats.hits==1);
    ps5vk_runtime_graphics_cached_release(cache,warm);ps5vk_runtime_graphics_cached_release(cache,cold);
    ps5vk_compilation_cache_destroy(cache);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    /* Both stages use each table with distinct coefficients. There must be a
     * separate compiler-selected argument slot in both register banks, but
     * only one table address per descriptor set. */
    key.vertex=read_module("build/runtime-graphics/shared_sets.vert.spv");
    key.fragment=read_module("build/runtime-graphics/shared_sets.frag.spv");
    for(unsigned s=0;s<4;++s)
        sets[s].binding[7].stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS);
    actual=compiled;
    assert(actual->vertex.machine_code_size && actual->fragment.machine_code_size);
    assert(!ps5vk_runtime_shader_build(&header,&actual->vertex));
    assert(!ps5vk_runtime_shader_build(&header,&actual->fragment));
    assert(!ps5vk_runtime_draw_values_sets(&actual->arguments,0,0,0,0,0,0,tables,vs,fs));
    for(unsigned s=0;s<4;++s) {
        assert(actual->arguments.vertex_descriptor_valid[s] &&
               actual->arguments.fragment_descriptor_valid[s]);
        assert(vs[actual->arguments.vertex_descriptor_slot[s]]==tables[s]);
        assert(fs[actual->arguments.fragment_descriptor_slot[s]]==tables[s]);
        assert(actual->vertex.metadata.descriptor_bindings[s].offset==0);
        assert(actual->fragment.metadata.descriptor_bindings[s].offset==0);
    }
    const VkShaderStageFlags visibility[]={VK_SHADER_STAGE_ALL,VK_SHADER_STAGE_ALL_GRAPHICS,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT};
    for(unsigned i=0;i<sizeof(visibility)/sizeof(visibility[0]);++i) {
        for(unsigned s=0;s<4;++s)sets[s].binding[7].stages=visibility[i];
        const void *wide=NULL;
        assert(ps5vk_runtime_graphics_compile(NULL,&key,&wide)==VK_SUCCESS);
        const struct ps5vk_runtime_graphics_program *candidate=wide;
        assert(candidate->vertex.machine_code_size==actual->vertex.machine_code_size &&
               candidate->fragment.machine_code_size==actual->fragment.machine_code_size);
        assert(!memcmp(candidate->vertex.machine_code,actual->vertex.machine_code,actual->vertex.machine_code_size));
        assert(!memcmp(candidate->fragment.machine_code,actual->fragment.machine_code,actual->fragment.machine_code_size));
        assert(!memcmp(&candidate->arguments,&actual->arguments,sizeof(actual->arguments)));
        for(unsigned s=0;s<4;++s) {
            assert(sets[s].binding[7].stages==visibility[i]);
            assert(candidate->vertex.metadata.descriptor_bindings[s].offset==0 &&
                   candidate->fragment.metadata.descriptor_bindings[s].offset==0);
        }
        ps5vk_runtime_graphics_free(NULL,wide);
    }
    sets[3].binding[7].stages=UINT32_C(0x40000000);
    assert(!ps5vk_runtime_graphics_supported(&key));
    ps5vk_runtime_graphics_free(NULL,compiled);
    free((void *)key.fragment.words);
    key.fragment=read_module("build/runtime-graphics/vertex_sets.frag.spv");
    for(unsigned s=0;s<4;++s)sets[s].binding[7].stages=VK_SHADER_STAGE_VERTEX_BIT;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS);
    actual=compiled;
    assert(!ps5vk_runtime_draw_values_sets(&actual->arguments,0,0,0,0,0,0,tables,vs,fs));
    for(unsigned s=0;s<4;++s) {
        assert(actual->arguments.vertex_descriptor_valid[s]);
        assert(!actual->arguments.fragment_descriptor_valid[s]);
        assert(vs[actual->arguments.vertex_descriptor_slot[s]]==tables[s]);
    }
    ps5vk_runtime_graphics_free(NULL,compiled);
    sets[3].binding[7].stages=VK_SHADER_STAGE_COMPUTE_BIT;
    /* Layout screening is not access validation: an absent stage may retain
     * a binding, but this VS actually reads it and compilation must refuse. */
    assert(ps5vk_runtime_graphics_supported(&key));
    compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)!=VK_SUCCESS);
    assert(!compiled);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    struct ps5vk_set_signature unused_set={0};
    unused_set.count=1;
    unused_set.binding[0]=(struct ps5vk_binding){.first=0,.count=1,
        .stages=VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT};
    unused_set.type[0]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for(unsigned b=1;b<PS5VK_MAX_BINDINGS;++b)unused_set.binding[b].first=1;
    struct ps5vk_graphics_key unused={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .descriptor_set_count=1,.descriptor_sets=&unused_set};
    assert(ps5vk_runtime_graphics_supported(&unused));
    compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&unused,&compiled)==VK_SUCCESS);
    const struct ps5vk_runtime_graphics_program *no_access=compiled;
    assert(!no_access->arguments.vertex_descriptor_valid[0]);
    assert(!no_access->arguments.fragment_descriptor_valid[0]);
    ps5vk_runtime_graphics_free(NULL,compiled);
    free((void *)unused.vertex.words);free((void *)unused.fragment.words);
    puts("Descriptor compiler: four sets / 96 array elements, fragment and shared-stage PSBC contracts");
}
static void check_interfaces(struct ps5vk_graphics_key *key)
{
    assert(ps5vk_spirv_graphics_interface(key));
    uint32_t *words=(void *)key->fragment.words;
    unsigned input=0,vector_tests=0,location_tests=0;
    for(size_t at=5;at<key->fragment.word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;unsigned op=w[0]&65535;
        if(op==59 && w[3]==1)input=w[2];
        if(op==23 && w[3]==3) {
            w[3]=2;
            assert(!ps5vk_spirv_graphics_interface(key));
            const void *rejected=(void *)1;
            assert(ps5vk_runtime_graphics_compile(NULL,key,&rejected)==VK_ERROR_FEATURE_NOT_PRESENT);
            assert(!rejected);
            w[3]=3;++vector_tests;
        }
    }
    assert(input && vector_tests);
    for(size_t at=5;at<key->fragment.word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;
        if((w[0]&65535)==71 && w[1]==input && w[2]==30) {
            unsigned saved=w[3];w[3]=31;
            assert(!ps5vk_spirv_graphics_interface(key)); /* missing VS location */
            w[3]=saved;w[2]=31;
            assert(!ps5vk_spirv_graphics_interface(key)); /* component packing */
            w[2]=30;++location_tests;
        }
    }
    assert(location_tests==1);
    unsigned bound=words[3];words[3]=65537;
    assert(!ps5vk_spirv_graphics_interface(key));words[3]=bound;
    unsigned first=words[5];words[5]=0;
    assert(!ps5vk_spirv_graphics_interface(key));words[5]=first;
    assert(ps5vk_spirv_graphics_interface(key));
}
static void check_flat_interfaces(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/flat.vert.spv"),
        .fragment=read_module("build/runtime-graphics/flat.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    const struct ps5vk_runtime_graphics_program *p=compiled;
    const PsbcShaderMetadata *fs=&p->fragment.metadata;
    assert(p->vertex.machine_code_size && p->fragment.machine_code_size);
    assert(fs->input_semantic_count==4);
    unsigned flat=0,smooth=0;
    for(unsigned i=0;i<fs->input_semantic_count;++i) {
        flat+=!!(fs->input_semantics[i]&(1u<<22));
        smooth+=!(fs->input_semantics[i]&(1u<<22));
    }
    assert(flat==3 && smooth==1); /* float, signed int, unsigned vector. */
    struct ps5vk_runtime_shader header;
    assert(!ps5vk_runtime_shader_build(&header,&p->fragment));
    assert(header.header.num_input_semantics==4 &&
        !memcmp(header.inputs,fs->input_semantics,4*sizeof(uint32_t)));
    struct ps5vk_runtime_draw_abi abi;
    assert(!ps5vk_runtime_draw_abi_build(&p->vertex.metadata,fs,&abi));
    ps5vk_runtime_graphics_free(NULL,compiled);

    uint32_t *words=(void *)key.fragment.words;
    unsigned flat_integer=0,flat_ids[4]={0};
    for(size_t at=5;at<key.fragment.word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;
        if((w[0]&65535)==71 && w[2]==30 && w[3]<4)flat_ids[w[3]]=w[1];
    }
    assert(flat_ids[1] && flat_ids[2] && flat_ids[3]);
    for(size_t at=5;at<key.fragment.word_count;at+=words[at]>>16) {
        uint32_t *w=words+at;
        if((w[0]&65535)!=71 || w[2]!=14)continue;
        assert((w[0]>>16)==3);
        w[2]=13; /* NoPerspective has not been qualified by this path. */
        assert(!ps5vk_spirv_graphics_interface(&key));
        w[2]=0; /* RelaxedPrecision: remove Flat without breaking SPIR-V shape. */
        if(w[1]==flat_ids[1])assert(ps5vk_spirv_graphics_interface(&key));
        else {
            assert(!ps5vk_spirv_graphics_interface(&key));
            const void *rejected=(void *)1;
            assert(ps5vk_runtime_graphics_compile(NULL,&key,&rejected)==VK_ERROR_FEATURE_NOT_PRESENT);
            assert(!rejected);++flat_integer;
        }
        w[2]=14;
        uint32_t opcode=w[0];w[0]=(4u<<16)|71u; /* Flat takes no operands. */
        assert(!ps5vk_spirv_graphics_interface(&key));w[0]=opcode;
    }
    assert(flat_integer==2 && ps5vk_spirv_graphics_interface(&key));
    /* Interpolation is chosen by FS; VS decorations need not be identical. */
    words=(void *)key.vertex.words;
    for(size_t at=5;at<key.vertex.word_count;at+=words[at]>>16)
        if((words[at]&65535)==71 && words[at+2]==14)words[at+2]=0;
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS);
    p=compiled;
    assert(p->fragment.metadata.input_semantic_count==4);
    ps5vk_runtime_graphics_free(NULL,compiled);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}
/* Every InputAttachmentIndex decoration (43) the module declares, with the
 * variable it decorates. Two subpassInput variables are two different
 * attachments only if their indices differ: the fragment input interface allows
 * at most one input variable per index and image aspect, so the fixture has to
 * declare distinct indices and this reads them out of the SPIR-V the test
 * compiles rather than trusting the GLSL text. */
static unsigned input_attachment_indices(const struct ps5vk_graphics_module_key *m,
    uint32_t ids[4],uint32_t indices[4])
{
    const uint32_t *w=(const uint32_t *)m->words;
    unsigned found=0;
    for(size_t at=5;at<m->word_count;at+=w[at]>>16) {
        if((w[at]&65535u)==71u && (w[at]>>16)==4u && w[at+2]==43u) {
            assert(found<4);
            ids[found]=w[at+1];indices[found]=w[at+3];++found;
        }
    }
    return found;
}

/* An input attachment is resource-only image data read by a fragment shader:
 * subpassLoad() goes through the attachment's own eight DWORD image record and
 * never through a sampler. The profile therefore admits the role as
 * fragment-visible only, projects it onto PSBC's resource-only type at the
 * canonical 32-byte stride - beside the 48-byte combined record of the same set
 * - and delivers its set through the same user-SGPR path a combined sampler
 * uses, while the combined behaviour itself stays what it was. */
static void check_input_attachment_descriptors(void)
{
    struct ps5vk_set_signature sets[2]={0};
    /* Set 0 mixes the two image roles, so the input attachment sits at the
     * combined record's canonical 48-byte offset; set 1 carries an input
     * attachment alone at offset zero. */
    sets[0].binding[1].count=1;sets[0].binding[1].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    sets[0].type[1]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sets[0].binding[3].count=1;sets[0].binding[3].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    sets[0].type[3]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    sets[1].binding[5].count=1;sets[1].binding[5].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    sets[1].type[5]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    for(unsigned s=0;s<2;++s) {
        uint32_t prefix=0;
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            sets[s].binding[b].first=prefix;
            prefix+=sets[s].binding[b].count;
        }
        sets[s].count=prefix;
    }
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/input_attachment.frag.spv"),
        .descriptor_set_count=2,.descriptor_sets=sets,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    assert(ps5vk_runtime_graphics_supported(&key));
    /* The options carry the canonical table, not a packed rewrite of it: the
     * combined pair keeps its 48 bytes and the resource-only role is exactly
     * the 32-byte record the table assigns it. */
    const PsbcDescriptorBinding expected[3]={
        {0,1,PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,1,0,48},
        {0,3,PSBC_DESCRIPTOR_INPUT_ATTACHMENT,1,48,32},
        {1,5,PSBC_DESCRIPTOR_INPUT_ATTACHMENT,1,0,32}};
    assert(ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)==
        expected[1].stride);
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_FRAGMENT,
        .entrypoint="main",.optimise=true,.address32_hi=2,.primitive_type=4,.rasterization_samples=1};
    assert(ps5vk_runtime_graphics_descriptor_options(&key,VK_SHADER_STAGE_FRAGMENT_BIT,&options)==VK_SUCCESS);
    assert(options.descriptor_binding_count==3);
    for(unsigned i=0;i<3;++i) {
        const PsbcDescriptorBinding *b=&options.descriptor_bindings[i];
        assert(b->set==expected[i].set && b->binding==expected[i].binding &&
            b->type==expected[i].type && b->array_size==expected[i].array_size &&
            b->offset==expected[i].offset && b->stride==expected[i].stride);
    }
    /* The role is fragment-only: the vertex projection of the same key names
     * none of the three bindings, so a subpass attachment can never be handed
     * to a stage that cannot read it. */
    PsbcCompileOptions vertex={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_VERTEX,
        .entrypoint="main",.optimise=true,.address32_hi=2,.primitive_type=4,.rasterization_samples=1};
    assert(ps5vk_runtime_graphics_descriptor_options(&key,VK_SHADER_STAGE_VERTEX_BIT,&vertex)==VK_SUCCESS);
    assert(!vertex.descriptor_binding_count);
    /* The real SPIR-V fixture goes through PSBC/ACO: nonempty pixel code plus
     * the exact descriptor metadata, and the static use the stage really has. */
    struct ps5vk_graphics_module_key module=read_module("build/runtime-graphics/input_attachment.frag.spv");
    /* The fixture reads two different attachments, so the SPIR-V carries two
     * InputAttachmentIndex decorations on two variables and the indices really
     * are 0 and 1 - not the same index twice, which the interface forbids per
     * image aspect however well the shader compiles. */
    uint32_t attachment_ids[4],attachment_indices[4];
    unsigned attachment_count=input_attachment_indices(&module,attachment_ids,attachment_indices);
    assert(attachment_count==2 && attachment_ids[0]!=attachment_ids[1]);
    assert(attachment_indices[0]!=attachment_indices[1]);
    assert((attachment_indices[0]==0 && attachment_indices[1]==1) ||
           (attachment_indices[0]==1 && attachment_indices[1]==0));
    PsbcShaderOutput output={0};
    assert(psbc_compile_shader(module.words,module.word_count*4,&options,&output)==PSBC_RESULT_OK);
    assert(output.machine_code && output.machine_code_size &&
        !(output.machine_code_size&3u) && output.metadata.hardware_stage==PSBC_HW_STAGE_PIXEL);
    assert(output.metadata.descriptor_binding_count==3);
    for(unsigned i=0;i<3;++i) {
        const PsbcDescriptorBinding *b=&output.metadata.descriptor_bindings[i];
        assert(b->set==expected[i].set && b->binding==expected[i].binding &&
            b->type==expected[i].type && b->array_size==expected[i].array_size &&
            b->offset==expected[i].offset && b->stride==expected[i].stride);
    }
    assert(output.metadata.descriptor_set_valid[0] && output.metadata.descriptor_set_valid[1]);
    assert(output.metadata.descriptor_used_binding_mask[0]==((UINT64_C(1)<<1)|(UINT64_C(1)<<3)));
    assert(output.metadata.descriptor_used_binding_mask[1]==(UINT64_C(1)<<5));
    /* The compiled fragment stage and the composite draw ABI agree on what the
     * shader uses: both sets are required, and the combined binding is still
     * used exactly as before beside the two resource-only ones. */
    const void *compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    const struct ps5vk_runtime_graphics_program *program=compiled;
    const PsbcShaderMetadata *fs=&program->fragment.metadata;
    assert(fs->descriptor_binding_count==3 &&
        fs->descriptor_used_binding_mask[0]==((UINT64_C(1)<<1)|(UINT64_C(1)<<3)) &&
        fs->descriptor_used_binding_mask[1]==(UINT64_C(1)<<5));
    assert(program->arguments.fragment_descriptor_valid[0] &&
        program->arguments.fragment_descriptor_valid[1] &&
        !program->arguments.vertex_descriptor_valid[0] &&
        !program->arguments.vertex_descriptor_valid[1]);
    assert(program->arguments.fragment_used_bindings[0]==((UINT64_C(1)<<1)|(UINT64_C(1)<<3)) &&
        program->arguments.fragment_used_bindings[1]==(UINT64_C(1)<<5));
    const uint32_t tables[PS5VK_MAX_SETS]={UINT32_C(0x1000),UINT32_C(0x2000),0,0};
    uint32_t vs[16],pixel[16];
    assert(!ps5vk_runtime_draw_values_sets(&program->arguments,0,0,0,0,0,0,tables,vs,pixel));
    for(unsigned s=0;s<2;++s)
        assert(pixel[program->arguments.fragment_descriptor_slot[s]]==tables[s]);
    ps5vk_runtime_graphics_free(NULL,compiled);
    /* Every refusal below leaves the caller's output untouched, and the PSBC
     * validation refuses a record whose width disagrees with its type: the
     * mutations reuse the accepted options and change exactly one field
     * (0 stride, 1 type, 2 array size, 3 offset, 4 set). */
    struct { unsigned binding, field; uint32_t value; } mutations[]={
        {1,0,48},{1,0,16},{1,0,64},{0,0,32},{0,0,16},
        {1,1,PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER},{1,1,PSBC_DESCRIPTOR_NONE},{1,1,99},
        {1,2,0},{1,3,8},{1,4,PSBC_MAX_DESCRIPTOR_SETS}};
    for(unsigned i=0;i<sizeof(mutations)/sizeof(mutations[0]);++i) {
        PsbcCompileOptions bad=options;
        PsbcDescriptorBinding *b=&bad.descriptor_bindings[mutations[i].binding];
        if(mutations[i].field==0)b->stride=mutations[i].value;
        else if(mutations[i].field==1)b->type=(PsbcDescriptorType)mutations[i].value;
        else if(mutations[i].field==2)b->array_size=mutations[i].value;
        else if(mutations[i].field==3)b->offset=mutations[i].value;
        else b->set=mutations[i].value;
        PsbcShaderOutput rejected={0};
        assert(psbc_compile_shader(module.words,module.word_count*4,&bad,&rejected)==
            PSBC_RESULT_INTERNAL_ERROR);
        assert(!rejected.machine_code && !rejected.machine_code_size && !rejected.metadata.version);
    }
    /* A duplicate (set, binding) pair is refused too, and an accepted compile
     * is not a licence for a malformed table: the second entry repeats the
     * first one's binding number. */
    PsbcCompileOptions duplicate=options;
    duplicate.descriptor_bindings[1].binding=duplicate.descriptor_bindings[0].binding;
    duplicate.descriptor_bindings[1].set=duplicate.descriptor_bindings[0].set;
    PsbcShaderOutput dup_out={0};
    assert(psbc_compile_shader(module.words,module.word_count*4,&duplicate,&dup_out)==
        PSBC_RESULT_INTERNAL_ERROR && !dup_out.machine_code);
    psbc_free_output(&output);
    free((void *)module.words);
    /* The profile gate is fail-closed before the compiler is reached: an input
     * attachment exposed to any stage other than the fragment stage is refused,
     * and the caller's options are left exactly as they were. */
    const VkShaderStageFlags refused[]={VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,VK_SHADER_STAGE_ALL,
        VK_SHADER_STAGE_COMPUTE_BIT};
    for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i) {
        struct ps5vk_set_signature widened[2]={0};
        memcpy(widened,sets,sizeof(sets));
        widened[0].binding[3].stages=refused[i];
        struct ps5vk_graphics_key bad_key=key;
        bad_key.descriptor_sets=widened;
        assert(!ps5vk_runtime_graphics_supported(&bad_key));
        PsbcCompileOptions saved=options;
        assert(ps5vk_runtime_graphics_descriptor_options(&bad_key,VK_SHADER_STAGE_FRAGMENT_BIT,
            &options)==VK_ERROR_FEATURE_NOT_PRESENT);
        assert(!memcmp(&saved,&options,sizeof(options)));
    }
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

/* Compile the exact two pipeline pairs embedded by the native witness.  This
 * catches interface-profile failures before deployment: in particular, the
 * pattern stage must use an ordinary VS->FS location rather than a fragment
 * built-in the bounded draw ABI does not expose. */
static void check_input_attachment_probe_pipelines(void)
{
    struct ps5vk_set_signature set={0};
    set.count=1;set.binding[0].count=1;set.binding[0].first=0;
    set.binding[0].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    set.type[0]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    for(unsigned binding=1;binding<PS5VK_MAX_BINDINGS;++binding)
        set.binding[binding].first=1;
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/input_attachment_probe.vert.spv"),
        .fragment=read_module("build/runtime-graphics/input_attachment_pattern.frag.spv"),
        .descriptor_set_count=1,.descriptor_sets=&set,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    const void *compiled=NULL;
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    const struct ps5vk_runtime_graphics_program *program=compiled;
    assert(!program->arguments.vertex_descriptor_valid[0]);
    assert(!program->arguments.fragment_descriptor_valid[0]);
    ps5vk_runtime_graphics_free(NULL,compiled);
    free((void *)key.fragment.words);

    key.fragment=read_module("build/runtime-graphics/input_attachment_transform.frag.spv");
    compiled=NULL;
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    program=compiled;
    assert(!program->arguments.vertex_descriptor_valid[0]);
    assert(program->arguments.fragment_descriptor_valid[0]);
    assert(program->arguments.fragment_used_bindings[0]==UINT64_C(1));
    ps5vk_runtime_graphics_free(NULL,compiled);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

static void check_fragment_store_atomic_contract(void)
{
    struct ps5vk_set_signature set={0};
    set.count=1;
    set.binding[0]=(struct ps5vk_binding){.count=1,.first=0,
        .stages=VK_SHADER_STAGE_FRAGMENT_BIT};
    set.type[0]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for(unsigned binding=1;binding<PS5VK_MAX_BINDINGS;++binding)
        set.binding[binding].first=1;
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/fragment_store.frag.spv"),
        .descriptor_set_count=1,.descriptor_sets=&set,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    const void *compiled=NULL;

    /* The descriptor profile can represent the resource, but shader side
     * effects are a separate core feature.  The same candidate must fail
     * before packaging until the logical device enabled it. */
    assert(ps5vk_runtime_graphics_supported(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==
        VK_ERROR_FEATURE_NOT_PRESENT && !compiled);
    key.feature_mask=PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    const struct ps5vk_runtime_graphics_program *program=compiled;
    assert(program->fragment.metadata.hardware_stage==PSBC_HW_STAGE_PIXEL);
    assert(program->fragment.metadata.descriptor_binding_count==1);
    assert(program->fragment.metadata.descriptor_used_binding_mask[0]==UINT64_C(1));
    assert(program->arguments.fragment_descriptor_valid[0]);
    assert(program->arguments.fragment_used_bindings[0]==UINT64_C(1));
    PsbcShaderMetadata candidate_fs=program->fragment.metadata;
    PsbcShaderMetadata candidate_vs=program->vertex.metadata;
    PsbcRegisterWrite *db=context_register(&candidate_fs,0x203u);
    assert(db && (db->value&UINT32_C(0x600))==UINT32_C(0x600));
    struct ps5vk_runtime_shader header;
    assert(!ps5vk_runtime_shader_build(&header,&program->fragment));
    assert(ps5vk_runtime_graphics_feature_use_ok(&candidate_vs,&candidate_fs,
        PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS));
    assert(!ps5vk_runtime_graphics_feature_use_ok(&candidate_vs,&candidate_fs,0));
    db->value&=~UINT32_C(0x200);
    assert(!ps5vk_runtime_graphics_feature_use_ok(&candidate_vs,&candidate_fs,
        PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS));
    db->value|=UINT32_C(0x200);db->value&=~UINT32_C(0x400);
    assert(!ps5vk_runtime_graphics_feature_use_ok(&candidate_vs,&candidate_fs,
        PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS));
    ps5vk_runtime_graphics_free(NULL,compiled);

    /* Both stages declare GLSL450. The base KHR memory-model bit may be
     * enabled on the logical device without changing this legacy fragment
     * atomic shader's PSBC memory-model interpretation. */
    key.feature_mask|=PS5VK_FEATURE_VULKAN_MEMORY_MODEL;
    compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    ps5vk_runtime_graphics_free(NULL,compiled);
    key.feature_mask=PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS;

    /* Match the focused upstream frag_side_effects shape: derive an SSBO
     * index from gl_FragCoord, store, then discard.  This composes the shared
     * fragment-position contract with the T06 side-effect contract and proves
     * that the exact pair reaches PSBC packaging. */
    free((void *)key.fragment.words);
    key.fragment=read_module("build/runtime-graphics/fragment_coord_store.frag.spv");
    compiled=NULL;
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    program=compiled;
    assert(program->fragment.metadata.hardware_stage==PSBC_HW_STAGE_PIXEL);
    assert(program->fragment.metadata.descriptor_used_binding_mask[0]==UINT64_C(1));
    assert(program->arguments.fragment_descriptor_valid[0]);
    ps5vk_runtime_graphics_free(NULL,compiled);

    /* The paired CTS leaf places its colour write after OpKill. Glslang keeps
     * the output in OpEntryPoint but removes the unreachable store. The
     * fragment side effect remains real and must compile with the same SSBO
     * contract; an absent colour export is not grounds to reject the stage. */
    free((void *)key.fragment.words);
    key.fragment=read_module(
        "build/runtime-graphics/fragment_coord_store_after_kill.frag.spv");
    compiled=NULL;
    assert(ps5vk_spirv_graphics_interface(&key));
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    program=compiled;
    assert(program->fragment.metadata.hardware_stage==PSBC_HW_STAGE_PIXEL);
    assert(program->fragment.metadata.descriptor_used_binding_mask[0]==UINT64_C(1));
    assert(program->arguments.fragment_descriptor_valid[0]);
    const PsbcRegisterWrite *color_mask=context_register(
        (PsbcShaderMetadata *)&program->fragment.metadata,0x8fu);
    assert(color_mask && color_mask->value==0);
    PsbcShaderOutput malformed_fragment=program->fragment;
    PsbcRegisterWrite *malformed_mask=context_register(
        &malformed_fragment.metadata,0x8fu);
    assert(malformed_mask);malformed_mask->value=1;
    assert(ps5vk_runtime_shader_build(&header,&malformed_fragment)!=0);
    ps5vk_runtime_graphics_free(NULL,compiled);

    /* Same layout, no store: the compiler removes the unused declaration,
     * leaves the DB writes-memory bits clear and needs no feature or descriptor
     * table.  This is the same-artifact negative control for the native slice. */
    free((void *)key.fragment.words);
    key.fragment=read_module("build/runtime-graphics/triangle.frag.spv");
    key.feature_mask=0;compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS && compiled);
    program=compiled;
    PsbcShaderMetadata control=program->fragment.metadata;
    db=context_register(&control,0x203u);
    assert(db && !(db->value&UINT32_C(0x600)));
    assert(!program->arguments.fragment_descriptor_valid[0]);
    assert(!program->arguments.fragment_used_bindings[0]);
    ps5vk_runtime_graphics_free(NULL,compiled);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
}

static void check_t08_compiler_options(void)
{
    const uint32_t glsl450[] = {0x07230203u, 0x00010000u, 0, 3, 0,
        (3u << 16) | 14u, 0, 1};
    const uint32_t vulkankhr[] = {0x07230203u, 0x00010000u, 0, 3, 0,
        (3u << 16) | 14u, 0, 3};
    const struct ps5vk_graphics_module_key legacy = {
        .words = glsl450, .word_count = sizeof(glsl450) / sizeof(glsl450[0])};
    const struct ps5vk_graphics_module_key khr = {
        .words = vulkankhr, .word_count = sizeof(vulkankhr) / sizeof(vulkankhr[0])};
    PsbcCompileOptions options = {.target = PSBC_TARGET_PS5,
        .sample_shading_enable = true};
    assert(ps5vk_runtime_graphics_t08_options(0, &legacy, &options) == VK_SUCCESS);
    assert(!options.enable_physical_storage_buffer_addresses &&
           !options.enable_vulkan_memory_model &&
           !options.enable_vulkan_memory_model_device_scope);
    assert(ps5vk_runtime_graphics_t08_options(
        PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE, &khr, &options) ==
        VK_ERROR_FEATURE_NOT_PRESENT);
    const uint32_t all = PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS |
        PS5VK_FEATURE_VULKAN_MEMORY_MODEL |
        PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE;
    assert(ps5vk_runtime_graphics_t08_options(all, &khr, &options) == VK_SUCCESS);
    assert(options.enable_physical_storage_buffer_addresses &&
           options.enable_vulkan_memory_model &&
           options.enable_vulkan_memory_model_device_scope);
    assert(options.target == PSBC_TARGET_PS5 && options.sample_shading_enable);
    assert(ps5vk_runtime_graphics_t08_options(PS5VK_FEATURE_VULKAN_MEMORY_MODEL,
                                              &khr, &options) == VK_SUCCESS);
    assert(!options.enable_physical_storage_buffer_addresses &&
           options.enable_vulkan_memory_model &&
           !options.enable_vulkan_memory_model_device_scope);
    /* A legacy stage on the same logical device retains GLSL450 barrier
     * semantics even when KHR memory-model features are enabled. */
    assert(ps5vk_runtime_graphics_t08_options(all, &legacy, &options) == VK_SUCCESS);
    assert(options.enable_physical_storage_buffer_addresses &&
           !options.enable_vulkan_memory_model &&
           !options.enable_vulkan_memory_model_device_scope);
    const uint32_t malformed[] = {0x07230203u, 0x00010000u, 0, 3, 0,
        (4u << 16) | 14u, 0, 3};
    const struct ps5vk_graphics_module_key bad = {
        .words = malformed, .word_count = sizeof(malformed) / sizeof(malformed[0])};
    assert(ps5vk_runtime_graphics_t08_options(all, &bad, &options) == VK_ERROR_UNKNOWN);
}

int main(void)
{
    check_t08_compiler_options();
    check_flat_interfaces();
    check_descriptor_options();
    check_input_attachment_descriptors();
    check_input_attachment_probe_pipelines();
    check_fragment_store_atomic_contract();
    check_sparse_layout_static_use();
    check_cube_array_feature_mask();
    check_view_index_builtin();
    check_clip_cull_distances();
    check_depth_only_target();
    check_fragment_distance_read();
    check_fragment_position();
    check_sample_rate_compilation();
    check_subpass_fetch_compilation();
    check_resolve_program();
    check_dual_source_exports();
    check_dual_source_blend_contract();
    check_two_mrt_exports();
    check_two_target_write_masks();
    check_second_target_only();
    check_gather_compiler_forms();
    check_geometry_stage();
    check_viewport_index_routing();
    check_geometry_output_components();
    check_geometry_stage_descriptor_visibility();
    check_tessellation_stage();
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    const void *out=NULL;
    check_interfaces(&key);
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p=out;
    assert(p->vertex.machine_code_size && p->fragment.machine_code_size);
    assert(p->vertex.metadata.hardware_stage==PSBC_HW_STAGE_NGG);
    assert(p->fragment.metadata.hardware_stage==PSBC_HW_STAGE_PIXEL);
    assert(p->arguments.base_vertex_slot==0 && p->arguments.lds_slot==1);
    ps5vk_runtime_graphics_free(NULL,out);

    struct ps5vk_set_signature sampled={0};
    sampled.count=1;sampled.binding[0].count=1;sampled.binding[0].first=0;
    sampled.binding[0].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    sampled.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for(unsigned binding_index=1;binding_index<PS5VK_MAX_BINDINGS;++binding_index)
        sampled.binding[binding_index].first=1;
    struct ps5vk_graphics_key textured=key;
    textured.vertex=read_module("build/runtime-graphics/mipmap.vert.spv");
    textured.fragment=read_module("build/runtime-graphics/texture.frag.spv");
    VkVertexInputBindingDescription mip_binding={.binding=0,.stride=24,
        .inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription mip_attributes[2]={
        {.location=0,.binding=0,.format=VK_FORMAT_R32G32B32_SFLOAT,.offset=0},
        {.location=1,.binding=0,.format=VK_FORMAT_R32G32B32_SFLOAT,.offset=12}};
    textured.vertex_binding_count=1;textured.vertex_attribute_count=2;
    textured.vertex_bindings=&mip_binding;textured.vertex_attributes=mip_attributes;
    textured.descriptor_set_count=1;textured.descriptor_sets=&sampled;
    assert(ps5vk_runtime_graphics_compile(NULL,&textured,&out)==VK_SUCCESS && out);
    p=out;
    assert(p->fragment.metadata.descriptor_binding_count==1);
    assert(p->fragment.metadata.descriptor_set0_valid &&
        p->fragment.metadata.descriptor_set_valid[0]);
    assert(p->arguments.fragment_descriptor_valid[0] &&
        p->arguments.fragment_descriptor_slot[0]< p->arguments.fragment_count);
    ps5vk_runtime_graphics_free(NULL,out);
    sampled.binding[0].stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    assert(ps5vk_runtime_graphics_compile(NULL,&textured,&out)==VK_SUCCESS && out);
    p=out;
    /* The layout declares vertex and fragment visibility, but only the fragment
     * shader dereferences the sampler: the vertex stage must not reserve a
     * descriptor pointer for a binding it never reads. */
    assert(!p->arguments.vertex_descriptor_valid[0] &&
            p->arguments.fragment_descriptor_valid[0]);
    for(unsigned s=0;s<PS5VK_MAX_SETS;++s)
        assert(!p->arguments.vertex_descriptor_valid[s]);
    ps5vk_runtime_graphics_free(NULL,out);
    sampled.binding[0].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    sampled.type[0]=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    assert(ps5vk_runtime_graphics_compile(NULL,&textured,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    sampled.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    free((void *)textured.vertex.words);free((void *)textured.fragment.words);
    struct ps5vk_compilation_cache *cache=ps5vk_compilation_cache_create(4,1024*1024);
    assert(cache);
    const void *cold,*warm;
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&cold)==VK_SUCCESS);
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&warm)==VK_SUCCESS);
    p=cold;
    assert(p->vertex.machine_code==((const struct ps5vk_runtime_graphics_program *)warm)->vertex.machine_code);
    struct ps5vk_cache_stats stats;
    ps5vk_compilation_cache_get_stats(cache,&stats);
    assert(stats.compiles==1 && stats.hits==1 && stats.misses==1 && stats.current_entries==1);
    ps5vk_runtime_graphics_cached_release(cache,warm);
    /* Each module independently participates in identity. Generator words can
     * differ without invalidating this owned SPIR-V or changing its semantics. */
    for(unsigned stage=0;stage<2;++stage) {
        uint32_t *words=(uint32_t *)(stage?key.fragment.words:key.vertex.words);
        words[2]^=1;
        assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&warm)==VK_SUCCESS);
        ps5vk_runtime_graphics_cached_release(cache,warm);
        words[2]^=1;
    }
    ps5vk_compilation_cache_get_stats(cache,&stats);
    assert(stats.compiles==3 && stats.current_entries==3);
    /* Blending is part of the served contract now, so an unblended pipeline
     * switching to the all-zero factors is a different program, not a refusal. */
    key.blend_enable[0]=1;
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_cached_release(cache,out);
    key.blend_enable[0]=0;
    ps5vk_compilation_cache_get_stats(cache,&stats);
    assert(stats.compiles==4u && stats.hits==1);
    ps5vk_compilation_cache_destroy(cache);
    /* View remains valid until its detached lease is released. */
    assert(p->vertex.machine_code_size && p->arguments.lds_slot==1);
    ps5vk_runtime_graphics_cached_release(NULL,cold);

    struct ps5vk_graphics_key parameters={
        .vertex=read_module("build/runtime-graphics/parameters.vert.spv"),
        .fragment=read_module("build/runtime-graphics/parameters.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},.push_constant_size=16};
    for(unsigned i=0;i<4;++i)parameters.push_constant_stages[i]=
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    float vertex_scale=0.75f,fragment_intensity=0.5f;
    parameters.vertex.specialization_count=1;
    parameters.vertex.specializations[0].constant_id=0;
    parameters.vertex.specializations[0].size=sizeof(vertex_scale);
    memcpy(parameters.vertex.specializations[0].data,&vertex_scale,sizeof(vertex_scale));
    parameters.fragment.specialization_count=1;
    parameters.fragment.specializations[0].constant_id=1;
    parameters.fragment.specializations[0].size=sizeof(fragment_intensity);
    memcpy(parameters.fragment.specializations[0].data,&fragment_intensity,sizeof(fragment_intensity));
    assert(ps5vk_runtime_graphics_compile(NULL,&parameters,&out)==VK_SUCCESS && out);
    p=out;
    assert(p->arguments.push_constant_size && p->arguments.push_constant_size<=16 &&
        p->arguments.vertex_push_slot!=UINT32_MAX &&
        p->arguments.fragment_push_slot!=UINT32_MAX);
    ps5vk_runtime_graphics_free(NULL,out);

    VkVertexInputBindingDescription binding={.binding=0,.stride=16,
        .inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute={.location=0,.binding=0,
        .format=VK_FORMAT_R32G32B32A32_SFLOAT,.offset=0};
    struct ps5vk_graphics_key vertex_input={
        .vertex=read_module("build/runtime-graphics/vertex_input.vert.spv"),
        .fragment=read_module("build/runtime-graphics/vertex_input.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .vertex_binding_count=1,.vertex_attribute_count=1,
        .vertex_bindings=&binding,.vertex_attributes=&attribute};
    assert(ps5vk_spirv_graphics_interface(&vertex_input));
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    attribute.format=VK_FORMAT_R32G32B32_SFLOAT;
    VkVertexInputBindingDescription many_bindings[16];
    for(unsigned i=0;i<16;++i)many_bindings[i]=(VkVertexInputBindingDescription){15-i,16,VK_VERTEX_INPUT_RATE_VERTEX};
    vertex_input.vertex_binding_count=16;vertex_input.vertex_bindings=many_bindings;
    attribute.binding=15;
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_SUCCESS && out);
    p=out;
    assert(p->vertex.metadata.vertex_buffer_usage_mask==0x8000 &&
        !p->vertex.metadata.vertex_buffer_per_attribute && p->arguments.vertex_buffer_usage_mask==0x8000);
    ps5vk_runtime_graphics_free(NULL,out);
    many_bindings[0].binding=16;
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    many_bindings[0].binding=14;
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    vertex_input.vertex_binding_count=1;vertex_input.vertex_bindings=&binding;attribute.binding=0;
    struct ps5vk_graphics_key multi=vertex_input;
    multi.vertex=read_module("build/runtime-graphics/vertex_bindings.vert.spv");
    VkVertexInputAttributeDescription many_attributes[16];
    for(unsigned i=0;i<16;++i) {
        many_bindings[i]=(VkVertexInputBindingDescription){15-i,16,VK_VERTEX_INPUT_RATE_VERTEX};
        many_attributes[i]=(VkVertexInputAttributeDescription){i,15-i,VK_FORMAT_R32G32B32A32_SFLOAT,0};
    }
    multi.vertex_binding_count=multi.vertex_attribute_count=16;
    multi.vertex_bindings=many_bindings;multi.vertex_attributes=many_attributes;
    cache=ps5vk_compilation_cache_create(4,1024*1024);
    for(unsigned variant=0;variant<2;++variant) {
        multi.vertex.specialization_count=variant;
        multi.vertex.specializations[0]=(struct ps5vk_graphics_specialization){.constant_id=0,.size=4};
        assert(ps5vk_runtime_graphics_cached_acquire(cache,&multi,&cold)==VK_SUCCESS);
        assert(ps5vk_runtime_graphics_cached_acquire(cache,&multi,&warm)==VK_SUCCESS);
        p=warm;
        assert(p->arguments.vertex_buffer_usage_mask==(variant?0x8000:0xffff));
        assert(p->vertex.metadata.vertex_buffer_usage_mask==p->arguments.vertex_buffer_usage_mask);
        ps5vk_runtime_graphics_cached_release(cache,warm);ps5vk_runtime_graphics_cached_release(cache,cold);
    }
    ps5vk_compilation_cache_destroy(cache);free((void *)multi.vertex.words);
    multi.vertex=read_module("build/runtime-graphics/vertex_bindings_probe.vert.spv");
    for(unsigned i=0;i<16;++i) {
        unsigned b=15-i;
        many_bindings[i].stride=48+4*b;
        many_attributes[i].offset=4*(b%3);
    }
    cache=ps5vk_compilation_cache_create(4,1024*1024);
    for(unsigned variant=0;variant<4;++variant) {
        multi.vertex.specialization_count=1;
        multi.vertex.specializations[0]=(struct ps5vk_graphics_specialization){
            .constant_id=0,.size=4,.data={variant%3}};
        assert(ps5vk_runtime_graphics_cached_acquire(cache,&multi,&cold)==VK_SUCCESS);
        p=cold;
        const uint32_t masks[3]={0xffff,0x8000,0x8008};
        assert(p->arguments.vertex_buffer_usage_mask==masks[variant%3]);
        ps5vk_runtime_graphics_cached_release(cache,cold);
    }
    ps5vk_compilation_cache_get_stats(cache,&stats);
    assert(stats.compiles==3 && stats.hits==1);
    ps5vk_compilation_cache_destroy(cache);free((void *)multi.vertex.words);
    assert(ps5vk_spirv_graphics_interface(&vertex_input));
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    attribute.format=VK_FORMAT_R32_UINT;
    assert(!ps5vk_spirv_graphics_interface(&vertex_input));
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    attribute.format=VK_FORMAT_R32G32B32A32_SFLOAT;
    /* Same SPIR-V, distinct baked vertex layouts: each change must compile
     * separately, while repeating the exact layout must reuse its entry. */
    cache=ps5vk_compilation_cache_create(8,1024*1024);
    assert(cache);
    for(unsigned variant=0;variant<4;++variant) {
        binding.stride=variant?20:16;
        attribute.offset=variant>=2?4:0;
        attribute.format=variant==3?VK_FORMAT_R32G32B32_SFLOAT:VK_FORMAT_R32G32B32A32_SFLOAT;
        assert(ps5vk_runtime_graphics_cached_acquire(cache,&vertex_input,&cold)==VK_SUCCESS && cold);
        assert(ps5vk_runtime_graphics_cached_acquire(cache,&vertex_input,&warm)==VK_SUCCESS && warm);
        ps5vk_compilation_cache_get_stats(cache,&stats);
        assert(stats.misses==variant+1 && stats.hits==variant+1 && stats.current_entries==variant+1);
        ps5vk_runtime_graphics_cached_release(cache,warm);
        ps5vk_runtime_graphics_cached_release(cache,cold);
    }
    binding.stride=16;attribute.offset=0;attribute.format=VK_FORMAT_R32G32B32A32_SFLOAT;
    attribute.location=32;
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&vertex_input,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    attribute.location=0;binding.binding=16;
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&vertex_input,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    binding.binding=0;
    ps5vk_compilation_cache_destroy(cache);
    binding.inputRate=VK_VERTEX_INPUT_RATE_INSTANCE;
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    free((void *)vertex_input.vertex.words);free((void *)vertex_input.fragment.words);

    const char *integer_modules[]={"build/runtime-graphics/vertex_sint.vert.spv",
        "build/runtime-graphics/vertex_uint.vert.spv"};
    const VkFormat integer_formats[][4]={
        {VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32_SINT,
         VK_FORMAT_R32G32B32A32_SINT},
        {VK_FORMAT_R32_UINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32B32_UINT,
         VK_FORMAT_R32G32B32A32_UINT},
    };
    for(unsigned kind=0;kind<2;++kind) {
        struct ps5vk_graphics_key integer_input={
            .vertex=read_module(integer_modules[kind]),
            .fragment=read_module("build/runtime-graphics/vertex_input.frag.spv"),
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
            .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
            .vertex_binding_count=1,.vertex_attribute_count=1,
            .vertex_bindings=&binding,.vertex_attributes=&attribute};
        binding.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
        for(unsigned components=0;components<4;++components) {
            attribute.format=integer_formats[kind][components];
            binding.stride=4u*(components+1u);
            assert(ps5vk_spirv_graphics_interface(&integer_input));
            assert(ps5vk_runtime_graphics_compile(NULL,&integer_input,&out)==VK_SUCCESS && out);
            ps5vk_runtime_graphics_free(NULL,out);
        }
        free((void *)integer_input.vertex.words);
        free((void *)integer_input.fragment.words);
    }

    struct ps5vk_graphics_key packed_input={
        .vertex=read_module("build/runtime-graphics/vertex_unorm.vert.spv"),
        .fragment=read_module("build/runtime-graphics/vertex_input.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .vertex_binding_count=1,.vertex_attribute_count=1,
        .vertex_bindings=&binding,.vertex_attributes=&attribute};
    binding.stride=4;
    const VkFormat packed_formats[]={VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    for(unsigned i=0;i<3;++i) {
        attribute.format=packed_formats[i];
        assert(ps5vk_spirv_graphics_interface(&packed_input));
        assert(ps5vk_runtime_graphics_compile(NULL,&packed_input,&out)==VK_SUCCESS && out);
        ps5vk_runtime_graphics_free(NULL,out);
    }
    free((void *)packed_input.vertex.words);free((void *)packed_input.fragment.words);

    struct ps5vk_graphics_module_key probe_modules[3]={
        read_module("build/runtime-graphics/vertex_sint.vert.spv"),
        read_module("build/runtime-graphics/vertex_uint.vert.spv"),
        read_module("build/runtime-graphics/vertex_unorm.vert.spv")};
    struct ps5vk_graphics_module_key probe_fragment=
        read_module("build/runtime-graphics/vertex_input.frag.spv");
    for(unsigned i=0;i<PS5VK_VERTEX_FORMAT_CASES;++i) {
        struct ps5vk_vertex_format_case c;
        assert(!ps5vk_vertex_format_case(i,&c));
        unsigned module=c.numeric==PS5VK_VERTEX_PROBE_SINT?0:
            (c.numeric==PS5VK_VERTEX_PROBE_UINT?1:2);
        binding.stride=c.bytes;binding.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
        attribute.format=c.format;attribute.offset=0;
        struct ps5vk_graphics_key probe_input={
            .vertex=probe_modules[module],.fragment=probe_fragment,
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .color_format={VK_FORMAT_R8G8B8A8_UNORM},.color_attachment_count=1,
            .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
            .vertex_binding_count=1,.vertex_attribute_count=1,
            .vertex_bindings=&binding,.vertex_attributes=&attribute};
        assert(ps5vk_spirv_graphics_interface(&probe_input));
        assert(ps5vk_runtime_graphics_compile(NULL,&probe_input,&out)==VK_SUCCESS && out);
        ps5vk_runtime_graphics_free(NULL,out);
    }
    for(unsigned i=0;i<3;++i)free((void *)probe_modules[i].words);
    free((void *)probe_fragment.words);

    cache=ps5vk_compilation_cache_create(4,1024*1024);
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&parameters,&cold)==VK_SUCCESS);
    vertex_scale=1.25f;
    memcpy(parameters.vertex.specializations[0].data,&vertex_scale,sizeof(vertex_scale));
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&parameters,&warm)==VK_SUCCESS);
    ps5vk_compilation_cache_get_stats(cache,&stats);
    assert(stats.compiles==2 && stats.misses==2 && stats.current_entries==2);
    ps5vk_runtime_graphics_cached_release(cache,warm);
    ps5vk_runtime_graphics_cached_release(cache,cold);
    ps5vk_compilation_cache_destroy(cache);
    free((void *)parameters.vertex.words);free((void *)parameters.fragment.words);

    cache=ps5vk_compilation_cache_create(1,1);
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&out)==VK_ERROR_OUT_OF_HOST_MEMORY && !out);
    ps5vk_compilation_cache_destroy(cache);
    key.fragment.entry="absent";
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
    key.fragment.entry="main";key.descriptor_set_count=1;key.descriptor_sets=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
    key.descriptor_set_count=0;key.blend_enable[0]=1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);out=NULL;
    /* The native TES/GS upstream overlap oracle needs this exact additive
     * shape without an experimental build flag. Keep other shapes refused. */
    struct ps5vk_graphics_key additive=key;
    additive.src_color_blend_factor[0]=VK_BLEND_FACTOR_SRC_ALPHA;
    additive.dst_color_blend_factor[0]=VK_BLEND_FACTOR_ONE;
    additive.color_blend_op[0]=VK_BLEND_OP_ADD;
    additive.src_alpha_blend_factor[0]=VK_BLEND_FACTOR_SRC_ALPHA;
    additive.dst_alpha_blend_factor[0]=VK_BLEND_FACTOR_ONE;
    additive.alpha_blend_op[0]=VK_BLEND_OP_ADD;
    assert(ps5vk_runtime_graphics_supported(&additive));
    assert(ps5vk_runtime_graphics_compile(NULL,&additive,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);out=NULL;
    additive.color_blend_op[0]=VK_BLEND_OP_SUBTRACT;
    assert(ps5vk_runtime_graphics_supported(&additive));
    additive.color_blend_op[0]=VK_BLEND_OP_ADD;
    additive.dst_alpha_blend_factor[0]=VK_BLEND_FACTOR_ZERO;
    assert(ps5vk_runtime_graphics_supported(&additive));
    key.blend_enable[0]=0;
    /* Topology selects the primitive the composite pipeline links, so the key
     * carries it and the compiler is asked for the matching value. Every
     * accepted topology compiles; everything else stays fail-closed, before the
     * compiler is reached. */
    const VkPrimitiveTopology unsupported_topologies[]={
        /* Point and line topologies resolve, but a pipeline WITHOUT a geometry
         * stage has no witness for rasterizing them directly, so they are
         * refused like the families the resolver does not carry at all. */
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY};
    /* PATCH_LIST resolves now (the tessellation draw's DI_PT_PATCH), but its
     * pipeline stands or falls on the tessellation contract, not on this
     * plain-pipeline resolver list. */
    uint32_t primitive_type=0;
    assert(ps5vk_agc_primitive_type(VK_PRIMITIVE_TOPOLOGY_POINT_LIST,&primitive_type)==0 &&
           primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_POINT_LIST);
    assert(ps5vk_agc_primitive_type(VK_PRIMITIVE_TOPOLOGY_LINE_LIST,&primitive_type)==0 &&
           primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_LINE_LIST);
    assert(ps5vk_agc_primitive_type(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,&primitive_type)==0 &&
           primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_LINE_STRIP);
    assert(ps5vk_agc_primitive_type(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,&primitive_type)==0 &&
           primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST);
    assert(ps5vk_agc_primitive_type(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,&primitive_type)==0 &&
           primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP);
    /* The resolver carries points and lines (they feed a geometry stage), while
     * the plain-pipeline list above is refused for a different reason: no
     * geometry stage. The native link gate names the resolver's whole set, so a
     * value the key resolves can never be refused by the linker's own check. */
    for(unsigned i=0;i<sizeof(unsupported_topologies)/sizeof(unsupported_topologies[0]);++i) {
        const int resolved=!ps5vk_agc_primitive_type(unsupported_topologies[i],&primitive_type);
        assert(!resolved || ps5vk_agc_primitive_needs_geometry(primitive_type));
        primitive_type=0;
    }
    assert(ps5vk_agc_primitive_linkable(PS5VK_AGC_PRIMITIVE_TYPE_POINT_LIST) &&
           ps5vk_agc_primitive_linkable(PS5VK_AGC_PRIMITIVE_TYPE_LINE_LIST) &&
           ps5vk_agc_primitive_linkable(PS5VK_AGC_PRIMITIVE_TYPE_LINE_STRIP) &&
           ps5vk_agc_primitive_linkable(PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST) &&
           ps5vk_agc_primitive_linkable(PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP) &&
           !ps5vk_agc_primitive_linkable(0u) && !ps5vk_agc_primitive_linkable(5u) &&
           !ps5vk_agc_primitive_linkable(9u) && !ps5vk_agc_primitive_linkable(12u));
    key.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    assert(ps5vk_runtime_graphics_supported(&key) && ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    assert(((const struct ps5vk_runtime_graphics_program *)out)->primitive_type==
        PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP);
    ps5vk_runtime_graphics_free(NULL,out);out=NULL;
    for(unsigned i=0;i<sizeof(unsupported_topologies)/sizeof(unsupported_topologies[0]);++i) {
        key.topology=unsupported_topologies[i];
        assert(!ps5vk_runtime_graphics_supported(&key) &&
               ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    }
    key.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    key.vertex.word_count--;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    puts("Runtime graphics compiler: pass (real VS/FS, metadata ABI, unsupported profiles)");
}

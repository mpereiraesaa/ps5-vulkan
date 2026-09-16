#include "runtime_graphics_compiler.h"
#include "compilation_cache.h"
#include "spirv_graphics_interface.h"
#include "vertex_format_probe.h"
#include "descriptor_table_layout.h"
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

/* ViewIndex is delivered to both stages through independently declared slots.
 * It is not a vertex attribute and does not admit other unsupported built-ins. */
static void check_view_index_builtin(void)
{
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/view_index.vert.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
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
    /* ViewIndex is a vertex input built-in, never an attribute: a key that
     * declared an attribute for it could not be matched to the module. */
    VkVertexInputBindingDescription binding={.binding=0,.stride=4,
        .inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute={.location=0,.binding=0,
        .format=VK_FORMAT_R32_SINT,.offset=0};
    struct ps5vk_graphics_key attributed=key;
    attributed.vertex_binding_count=1;attributed.vertex_attribute_count=1;
    attributed.vertex_bindings=&binding;attributed.vertex_attributes=&attribute;
    assert(!ps5vk_spirv_graphics_interface(&attributed));
    assert(ps5vk_runtime_graphics_compile(NULL,&attributed,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);

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
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
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
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
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
    key.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;key.color_format=VK_FORMAT_B8G8R8A8_UNORM;
    key.samples=VK_SAMPLE_COUNT_1_BIT;key.color_write_mask=15;
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
    assert(!ps5vk_runtime_graphics_supported(&key));
    free((void *)key.vertex.words);free((void *)key.fragment.words);
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
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
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
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
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
        .color_format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
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

int main(void)
{
    check_flat_interfaces();
    check_descriptor_options();
    check_input_attachment_descriptors();
    check_input_attachment_probe_pipelines();
    check_sparse_layout_static_use();
    check_view_index_builtin();
    struct ps5vk_graphics_key key={
        .vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
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
    key.blend_enable=1;
    assert(ps5vk_runtime_graphics_cached_acquire(cache,&key,&out)!=VK_SUCCESS && !out);
    key.blend_enable=0;
    ps5vk_compilation_cache_get_stats(cache,&stats);
    assert(stats.compiles==3 && stats.hits==1);
    ps5vk_compilation_cache_destroy(cache);
    /* View remains valid until its detached lease is released. */
    assert(p->vertex.machine_code_size && p->arguments.lds_slot==1);
    ps5vk_runtime_graphics_cached_release(NULL,cold);

    struct ps5vk_graphics_key parameters={
        .vertex=read_module("build/runtime-graphics/parameters.vert.spv"),
        .fragment=read_module("build/runtime-graphics/parameters.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15,.push_constant_size=16};
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
        .color_format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15,
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
            .color_format=VK_FORMAT_R8G8B8A8_UNORM,
            .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15,
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
        .color_format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15,
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
            .color_format=VK_FORMAT_R8G8B8A8_UNORM,
            .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15,
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
    key.descriptor_set_count=0;key.blend_enable=1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
    key.blend_enable=0;
    /* Topology selects the primitive the composite pipeline links, so the key
     * carries it and the compiler is asked for the matching value. Both
     * accepted topologies compile; everything else stays fail-closed, before
     * the compiler is reached. */
    uint32_t primitive_type=0;
    assert(ps5vk_agc_primitive_type(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,&primitive_type)==0 &&
           primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST);
    assert(ps5vk_agc_primitive_type(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,&primitive_type)==0 &&
           primitive_type==PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP);
    key.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    assert(ps5vk_runtime_graphics_supported(&key) && ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    assert(((const struct ps5vk_runtime_graphics_program *)out)->primitive_type==
        PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP);
    ps5vk_runtime_graphics_free(NULL,out);out=NULL;
    const VkPrimitiveTopology unsupported_topologies[]={
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST,VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY};
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

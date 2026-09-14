#include "runtime_graphics_compiler.h"
#include "compilation_cache.h"
#include "spirv_graphics_interface.h"
#include "vertex_format_probe.h"
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
    assert(!ps5vk_runtime_draw_values_sets(&p->arguments,0,0,
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
    assert(!ps5vk_runtime_draw_values_sets(&abi,0,0,0,0,tables,vs,fs));
    for(unsigned s=0;s<4;++s)assert(fs[abi.fragment_descriptor_slot[s]]==tables[s]);
    assert(ps5vk_runtime_draw_values(&abi,0,0,0,0,tables[0],vs,fs));
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
    assert(!ps5vk_runtime_draw_values_sets(&actual->arguments,0,0,0,0,tables,vs,fs));
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
    assert(!ps5vk_runtime_draw_values_sets(&actual->arguments,0,0,0,0,tables,vs,fs));
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
int main(void)
{
    check_flat_interfaces();
    check_descriptor_options();
    check_sparse_layout_static_use();
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
    key.blend_enable=0;key.vertex.word_count--;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    puts("Runtime graphics compiler: pass (real VS/FS, metadata ABI, unsupported profiles)");
}

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
int main(void)
{
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
    assert(p->arguments.fragment_descriptor_set0_valid &&
        p->arguments.fragment_descriptor_set0_slot< p->arguments.fragment_count);
    ps5vk_runtime_graphics_free(NULL,out);
    sampled.binding[0].stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    assert(ps5vk_runtime_graphics_compile(NULL,&textured,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
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
    assert(ps5vk_spirv_graphics_interface(&vertex_input));
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_SUCCESS && out);
    ps5vk_runtime_graphics_free(NULL,out);
    attribute.format=VK_FORMAT_R32_UINT;
    assert(!ps5vk_spirv_graphics_interface(&vertex_input));
    assert(ps5vk_runtime_graphics_compile(NULL,&vertex_input,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    attribute.format=VK_FORMAT_R32G32B32A32_SFLOAT;
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

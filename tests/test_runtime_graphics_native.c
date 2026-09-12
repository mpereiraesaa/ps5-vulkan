#define _GNU_SOURCE
#include "runtime_graphics_compiler.h"
#include "graphics_pipeline_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static size_t mapping_bytes;
static unsigned allocations,releases,agc_calls,fail_agc,fail_flush;
static VkResult allocate(void *ctx,VkDeviceSize size,void **address,void **backing)
{
    (void)ctx;mapping_bytes=((size_t)size+4095u)&~(size_t)4095u;
    void *p=mmap((void *)UINT64_C(0x200000000),mapping_bytes,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(p!=MAP_FAILED);*address=*backing=p;++allocations;return VK_SUCCESS;
}
static void release_memory(void *ctx,void *backing)
{ (void)ctx;assert(!munmap(backing,mapping_bytes));++releases; }
static VkResult flush(void *ctx,void *backing,VkDeviceSize offset,VkDeviceSize bytes)
{ (void)ctx;assert(backing && !offset && bytes<=mapping_bytes);return fail_flush?VK_ERROR_UNKNOWN:VK_SUCCESS; }
int32_t sceAgcCreateShader(void **out,void *storage,void *code)
{
    if(++agc_calls==fail_agc)return -1;
    struct ps5vk_runtime_shader *s=storage;
    assert(!((uintptr_t)code&255u));s->header.code=code;
    s->specials.draw_modifier=5;*out=s;return 0;
}
int32_t sceAgcLinkShaders(void *cx,void *uc,void *reserved,void *vs,void *fs,uint32_t primitive)
{
    assert(cx && uc && !reserved && vs && fs && primitive==4);
    return ++agc_calls==fail_agc?-1:0;
}
int ps5vk_graphics_pair_prepare(struct ps5vk_graphics_pair *p,void *image,size_t capacity,
    const struct ps5vk_graphics_pair_input *input)
{ (void)p;(void)image;(void)capacity;(void)input;assert(0);return -1; }
static struct ps5vk_graphics_module_key read_module(const char *path)
{
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);
    assert(bytes>0 && !(bytes%4));rewind(f);uint32_t *data=malloc((size_t)bytes);assert(data);
    assert(fread(data,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    return (struct ps5vk_graphics_module_key){data,(size_t)bytes/4,"main"};
}
int main(void)
{
    struct ps5vk_graphics_key key={.vertex=read_module("build/runtime-graphics/triangle.vert.spv"),
        .fragment=read_module("build/runtime-graphics/triangle.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15};
    const void *compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&compiled)==VK_SUCCESS);
    struct VkDevice_T device={.memory={NULL,allocate,release_memory,flush,flush}};
    void *state=NULL;
    for(fail_agc=1;fail_agc<=3;++fail_agc) {
        agc_calls=0;
        assert(ps5vk_native_runtime_graphics_create(&device,compiled,&state)!=VK_SUCCESS && !state);
        assert(allocations==releases);
    }
    fail_agc=0;fail_flush=1;agc_calls=0;
    assert(ps5vk_native_runtime_graphics_create(&device,compiled,&state)!=VK_SUCCESS && !state);
    assert(allocations==releases);
    fail_flush=0;agc_calls=0;
    assert(ps5vk_native_runtime_graphics_create(&device,compiled,&state)==VK_SUCCESS && state);
    struct ps5vk_native_graphics_pipeline *p=state;
    const struct ps5vk_runtime_graphics_program *source=compiled;
    assert(p->pair->ready && p->pair->runtime_arguments.enabled);
    assert(!memcmp(p->pair->runtime_vertex.header.code,source->vertex.machine_code,source->vertex.machine_code_size));
    assert(!memcmp(p->pair->runtime_fragment.header.code,source->fragment.machine_code,source->fragment.machine_code_size));
    ps5vk_runtime_graphics_free(NULL,compiled);
    /* GPU-owned copies and relative metadata outlive the compilation lease. */
    assert(p->pair->runtime_vertex.outputs[0]==15 && p->pair->runtime_fragment.inputs[0]==15);
    ps5vk_native_graphics_release(&device,state);assert(allocations==releases);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    puts("Runtime graphics native preparation: pass (mock AGC, copied ownership, rollback)");
}

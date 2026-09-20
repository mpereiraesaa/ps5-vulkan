#define _GNU_SOURCE
#include "runtime_graphics_compiler.h"
#include "graphics_pipeline_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static struct { void *address; size_t bytes; } mappings[16];
static uintptr_t next_address=UINT64_C(0x200000000);
static unsigned allocations,releases,agc_calls,fail_agc,fail_flush;
static uint32_t linked_primitive;
static VkResult allocate(void *ctx,VkDeviceSize size,void **address,void **backing)
{
    (void)ctx;size_t mapping_bytes=((size_t)size+4095u)&~(size_t)4095u;
    unsigned slot=0;while(slot<16 && mappings[slot].address)++slot;assert(slot<16);
    assert(next_address+mapping_bytes<UINT64_C(0x300000000));
    void *p=mmap((void *)next_address,mapping_bytes,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(p!=MAP_FAILED);next_address+=mapping_bytes;
    mappings[slot].address=p;mappings[slot].bytes=mapping_bytes;
    *address=*backing=p;++allocations;return VK_SUCCESS;
}
static void release_memory(void *ctx,void *backing)
{
    (void)ctx;unsigned slot=0;
    while(slot<16 && mappings[slot].address!=backing)++slot;
    assert(slot<16);
    assert(!munmap(backing,mappings[slot].bytes));mappings[slot].address=NULL;++releases;
}
static VkResult flush(void *ctx,void *backing,VkDeviceSize offset,VkDeviceSize bytes)
{
    (void)ctx;unsigned slot=0;
    while(slot<16 && mappings[slot].address!=backing)++slot;
    assert(slot<16);
    assert(backing && offset<=mappings[slot].bytes && bytes<=mappings[slot].bytes-offset);
    return fail_flush?VK_ERROR_UNKNOWN:VK_SUCCESS;
}
int32_t sceAgcCreateShader(void **out,void *storage,void *code)
{
    if(++agc_calls==fail_agc)return -1;
    struct ps5vk_runtime_shader *s=storage;
    assert(!((uintptr_t)code&255u));s->header.code=code;
    s->specials.draw_modifier=5;*out=s;return 0;
}
int32_t sceAgcLinkShaders(void *cx,void *uc,void *reserved,void *vs,void *fs,uint32_t primitive)
{
    assert(cx && uc && !reserved && vs && fs);
    linked_primitive=primitive;
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
    return (struct ps5vk_graphics_module_key){.words=data,.word_count=(size_t)bytes/4,.entry="main"};
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
        assert(ps5vk_native_runtime_graphics_create(&device,compiled,4u,&state)!=VK_SUCCESS && !state);
        assert(allocations==releases);
    }
    fail_agc=0;fail_flush=1;agc_calls=0;
    assert(ps5vk_native_runtime_graphics_create(&device,compiled,4u,&state)!=VK_SUCCESS && !state);
    assert(allocations==releases);
    fail_flush=0;agc_calls=0;
    assert(ps5vk_native_runtime_graphics_create(&device,compiled,4u,&state)==VK_SUCCESS && state);
    assert(linked_primitive==4u);
    struct ps5vk_native_graphics_pipeline *p=state;
    const struct ps5vk_runtime_graphics_program *source=compiled;
    assert(p->pair->ready && p->pair->runtime_arguments.enabled);
    {
        uint32_t mask=~0u;
        assert(ps5vk_native_graphics_used_sets(&device,state,&mask)==VK_SUCCESS && !mask);
        p->pair->hull_arguments.enabled=1;
        p->pair->hull_arguments.vertex_descriptor_valid[2]=1;
        assert(ps5vk_native_graphics_used_sets(&device,state,&mask)==VK_SUCCESS && mask==4);
        p->pair->hull_arguments.vertex_used_bindings[2]=1;
        p->pair->runtime_arguments.fragment_descriptor_valid[1]=1;
        assert(ps5vk_native_graphics_used_sets(&device,state,&mask)==VK_SUCCESS && mask==6);
        p->pair->runtime_arguments.fragment_descriptor_valid[1]=0;
        memset(&p->pair->hull_arguments,0,sizeof(p->pair->hull_arguments));
        assert(ps5vk_native_graphics_used_sets(NULL,state,&mask)!=VK_SUCCESS && !mask);
    }
    assert(!memcmp(p->pair->runtime_vertex.header.code,source->vertex.machine_code,source->vertex.machine_code_size));
    assert(!memcmp(p->pair->runtime_fragment.header.code,source->fragment.machine_code,source->fragment.machine_code_size));
    ps5vk_runtime_graphics_free(NULL,compiled);
    /* GPU-owned copies and relative metadata outlive the compilation lease. */
    assert(p->pair->runtime_vertex.outputs[0]==15 && p->pair->runtime_fragment.inputs[0]==15);
    ps5vk_native_graphics_release(&device,state);assert(allocations==releases);
    /* A strip pipeline must program the strip primitive, and the pair it links
     * must be the pair that was compiled for that primitive: the program records
     * what it was compiled for and the backend refuses any other value before it
     * allocates or links. The program supplies code and the user-SGPR ABI; the
     * primitive is the pipeline's, which is why it is an argument. */
    struct ps5vk_graphics_key strip_key=key;
    strip_key.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    const void *strip_compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&strip_key,&strip_compiled)==VK_SUCCESS);
    assert(((const struct ps5vk_runtime_graphics_program *)strip_compiled)->primitive_type==6u);
    const void *list_compiled=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&list_compiled)==VK_SUCCESS);
    assert(((const struct ps5vk_runtime_graphics_program *)list_compiled)->primitive_type==4u);
    void *strip_state=NULL;
    unsigned before=allocations;
    /* A list-compiled pair linked as a strip, a strip-compiled pair linked as a
     * list, and a primitive outside the accepted set must all be refused. */
    assert(ps5vk_native_runtime_graphics_create(&device,list_compiled,6u,&strip_state)==
        VK_ERROR_FEATURE_NOT_PRESENT && !strip_state && allocations==before);
    assert(ps5vk_native_runtime_graphics_create(&device,strip_compiled,6u,&strip_state)==
        VK_SUCCESS && strip_state);
    assert(linked_primitive==6u);
    ps5vk_native_graphics_release(&device,strip_state);assert(allocations==releases);
    strip_state=NULL;
    before=allocations; /* every refusal below must leave that count untouched */
    assert(ps5vk_native_runtime_graphics_create(&device,strip_compiled,5u,&strip_state)==
        VK_ERROR_FEATURE_NOT_PRESENT && !strip_state && allocations==before);
    assert(ps5vk_native_runtime_graphics_create(&device,strip_compiled,0u,&strip_state)==
        VK_ERROR_FEATURE_NOT_PRESENT && !strip_state && allocations==before);
    assert(ps5vk_native_runtime_graphics_create(&device,strip_compiled,4u,&strip_state)==
        VK_ERROR_FEATURE_NOT_PRESENT && !strip_state && allocations==before);
    ps5vk_runtime_graphics_free(NULL,strip_compiled);
    ps5vk_runtime_graphics_free(NULL,list_compiled);
    struct ps5vk_graphics_key tess={
        .vertex=read_module("build/runtime-graphics/tess.vert.spv"),
        .tess_control=read_module("build/runtime-graphics/tess.tesc.spv"),
        .tess_eval=read_module("build/runtime-graphics/tess.tese.spv"),
        .geometry=read_module("build/runtime-graphics/geometry_probe.geom.spv"),
        .fragment=read_module("build/runtime-graphics/tess.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,.patch_control_points=3,
        .color_format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .color_write_mask=15,.feature_mask=PS5VK_FEATURE_TESSELLATION_SHADER|
            PS5VK_FEATURE_GEOMETRY_SHADER};
    const void *tess_program=NULL;void *tess_first=NULL,*tess_second=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&tess,&tess_program)==VK_SUCCESS);
    for(fail_agc=1;fail_agc<=3;++fail_agc) {
        agc_calls=0;
        assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_first)!=VK_SUCCESS);
        assert(!tess_first && allocations==releases);
    }
    fail_agc=0;fail_flush=1;
    assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_first)!=VK_SUCCESS);
    assert(!tess_first && allocations==releases);fail_flush=0;
    assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_first)==VK_SUCCESS);
    struct ps5vk_native_graphics_pipeline *first=tess_first;
    assert(first->pair->tessellation && first->pair->runtime_arguments.ring_table_valid);
    assert(first->shared_rings && first->pair->tess_rings);
    /* Default build, without diagnostic ring/offchip switches: the storage
     * and emitted bounds must match the checked native lifecycle. */
    const uint32_t *ring_table=first->pair->tess_rings;
    assert(ring_table[22]==65536u*4u);
    assert(ring_table[26]==256u*32768u);
    assert(first->pair->tess_ring_state[0].value==65536u);
    assert(first->pair->tess_ring_state[1].value==255u);
    assert(first->pair->geometry_preraster);
    ((unsigned char *)first->pair->tess_rings)[0]=0x5a;
    const unsigned live_before=allocations-releases;
    fail_flush=1;
    assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_second)!=VK_SUCCESS);
    assert(!tess_second && allocations-releases==live_before);
    assert(((unsigned char *)first->pair->tess_rings)[0]==0x5a);fail_flush=0;
    assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_second)==VK_SUCCESS);
    struct ps5vk_native_graphics_pipeline *second=tess_second;
    assert(first->shared_rings==second->shared_rings &&
        first->pair->tess_rings==second->pair->tess_rings);
    assert(((unsigned char *)second->pair->tess_rings)[0]==0x5a);
    assert((second->pair->tess_state[0].value&(1u<<5))!=0); /* GS_EN preserved. */
    assert(((second->pair->tess_state[0].value>>3)&3u)==1u); /* TES-fed ES. */
    ps5vk_runtime_graphics_free(NULL,tess_program);
    ps5vk_native_graphics_release(&device,tess_first);
    assert(((unsigned char *)second->pair->tess_rings)[0]==0x5a);
    ps5vk_native_graphics_release(&device,tess_second);assert(allocations==releases);
    /* A GS may emit points even though TES supplies triangles. Mutate only
     * OutputTriangleStrip (29) to OutputPoints (27), preserving executable
     * instructions; compiling the resulting legal module supplies real ABI. */
    uint32_t *gs_words=(uint32_t *)tess.geometry.words;
    unsigned changed=0;
    for(size_t at=5;at<tess.geometry.word_count;at+=gs_words[at]>>16) {
        if((gs_words[at]&65535u)==16u && (gs_words[at]>>16)==3 &&
           gs_words[at+2]==29u) {gs_words[at+2]=27u;++changed;}
    }
    assert(changed==1);
    tess_program=NULL;tess_first=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&tess,&tess_program)==VK_SUCCESS);
    const struct ps5vk_runtime_graphics_program *point_program=tess_program;
    unsigned output_registers=0;
    for(unsigned i=0;i<point_program->domain.metadata.context_register_count;++i)
        if(point_program->domain.metadata.context_registers[i].offset==0x29b) {
            assert(point_program->domain.metadata.context_registers[i].value==0);
            ++output_registers;
        }
    assert(output_registers==1);
    assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_first)==VK_SUCCESS);
    first=tess_first;
    assert(first->pair->cx.vgt_gs_out_prim_type.offset==0x29b);
    assert(first->pair->cx.vgt_gs_out_prim_type.value==0);
    ps5vk_native_graphics_release(&device,tess_first);assert(allocations==releases);
    /* Corrupt the retained compiler contract, not executable shader bytes.
     * Native construction must reject instead of reverting to TES topology. */
    struct ps5vk_runtime_graphics_program *mutable_point=(void *)tess_program;
    for(unsigned i=0;i<mutable_point->domain.metadata.context_register_count;++i)
        if(mutable_point->domain.metadata.context_registers[i].offset==0x29b) {
            mutable_point->domain.metadata.context_registers[i].value=3;
            tess_first=(void *)(uintptr_t)1;
            assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_first)!=VK_SUCCESS);
            assert(!tess_first && allocations==releases);
            mutable_point->domain.metadata.context_registers[i].value=0;
            mutable_point->domain.metadata.context_registers[i].offset=0x29c;
            tess_first=(void *)(uintptr_t)1;
            assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_first)!=VK_SUCCESS);
            assert(!tess_first && allocations==releases);
            mutable_point->domain.metadata.context_registers[i].offset=0x29b;
            break;
        }
    tess_first=NULL;
    assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_first)==VK_SUCCESS);
    ps5vk_native_graphics_release(&device,tess_first);assert(allocations==releases);
    ps5vk_runtime_graphics_free(NULL,tess_program);
    free((void *)tess.vertex.words);free((void *)tess.tess_control.words);
    free((void *)tess.tess_eval.words);free((void *)tess.geometry.words);free((void *)tess.fragment.words);
    /* Exact shader combination used by native variant16: quad point TES feeds
     * a real point-input GS which moves and recolours the domain outputs. */
    tess.vertex=read_module("build/runtime-graphics/tess_coord.vert.spv");
    tess.tess_control=read_module("build/runtime-graphics/tess_quad.tesc.spv");
    tess.tess_eval=read_module("build/runtime-graphics/tess_points.tese.spv");
    tess.geometry=read_module("build/runtime-graphics/tess_points.geom.spv");
    tess.fragment=read_module("build/runtime-graphics/tess_coord.frag.spv");
    tess_program=NULL;tess_first=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&tess,&tess_program)==VK_SUCCESS);
    assert(ps5vk_native_runtime_graphics_create(&device,tess_program,9u,&tess_first)==VK_SUCCESS);
    first=tess_first;
    assert(first->pair->tessellation && first->pair->geometry_preraster);
    assert(first->pair->cx.vgt_gs_out_prim_type.value==0);
    assert(first->pair->runtime_arguments.ring_table_valid);
    ps5vk_native_graphics_release(&device,tess_first);assert(allocations==releases);
    ps5vk_runtime_graphics_free(NULL,tess_program);
    free((void *)tess.vertex.words);free((void *)tess.tess_control.words);
    free((void *)tess.tess_eval.words);free((void *)tess.geometry.words);free((void *)tess.fragment.words);
    free((void *)key.vertex.words);free((void *)key.fragment.words);
    puts("Runtime graphics native preparation: pass (mock AGC, copied ownership, rollback)");
}

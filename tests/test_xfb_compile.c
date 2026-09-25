/* Transform feedback through the runtime graphics compiler (DXVK262-T14):
 * the merged vertex+geometry capture program compiles with the no-GDS global
 * streamout lowering, its metadata describes exactly the declared buffers,
 * and the draw ABI reserves the streamout table slot the queue fills per
 * draw. Real modules, real pinned compiler, host target. */
#include "runtime_graphics_compiler.h"
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

static struct ps5vk_graphics_key capture_key(const char *geometry, uint32_t buffers)
{
    return (struct ps5vk_graphics_key){
        .vertex=read_module("build/runtime-graphics/xfb_capture.vert.spv"),
        .geometry=read_module(geometry),
        .fragment=read_module("build/runtime-graphics/xfb_capture.frag.spv"),
        .topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER,
        .transform_feedback_buffers=buffers,.rasterizer_discard=VK_TRUE};
}
static void free_key(struct ps5vk_graphics_key *key)
{
    free((void *)key->vertex.words);
    free((void *)key->geometry.words);
    free((void *)key->fragment.words);
}

int main(void)
{
    /* The DXVK pass-through shape: stream 0, buffer 0, stride 32. */
    struct ps5vk_graphics_key key=capture_key("build/runtime-graphics/xfb_capture.geom.spv",1u);
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p=out;
    const PsbcShaderMetadata *m=&p->vertex.metadata;
    assert(m->source_stage==PSBC_STAGE_GEOMETRY && m->hardware_stage==PSBC_HW_STAGE_NGG);
    assert(m->streamout_valid && m->streamout_enabled_stream_buffers_mask==1u);
    assert(m->streamout_strides_dwords[0]==8u && !m->streamout_strides_dwords[1]);
    assert(ps5vk_runtime_streamout_matches(m,1u) && !ps5vk_runtime_streamout_matches(m,3u) &&
           !ps5vk_runtime_streamout_matches(m,0u));
    assert(p->arguments.streamout_valid==1u &&
           p->arguments.streamout_slot==m->streamout_buffer_table_user_data_dword &&
           p->arguments.streamout_slot<p->arguments.vertex_count && !p->arguments.streamout_low);
    /* The table pointer reaches its slot, and never collides with another. */
    struct ps5vk_runtime_draw_abi abi=p->arguments;
    abi.streamout_low=0x12345670u;
    const uint32_t tables[PS5VK_RUNTIME_DESCRIPTOR_SETS]={0};
    uint32_t vertex[16],pixel[16];
    assert(!ps5vk_runtime_draw_values_sets(&abi,0,0,0,0,0,0,tables,vertex,pixel));
    assert(vertex[abi.streamout_slot]==0x12345670u);
    abi.streamout_low=0x12345678u; /* the table is 16-byte aligned */
    assert(ps5vk_runtime_draw_values_sets(&abi,0,0,0,0,0,0,tables,vertex,pixel));
    abi=p->arguments;abi.streamout_valid=0;abi.streamout_slot=0;abi.streamout_low=16u;
    assert(ps5vk_runtime_draw_values_sets(&abi,0,0,0,0,0,0,tables,vertex,pixel));
    ps5vk_runtime_graphics_free(NULL,out);

    /* The same capture module under a key that declares no capture compiles
     * with the compiler's default (GDS-ordered) lowering, whose streamout the
     * key does not describe: refused, never executed. */
    key.transform_feedback_buffers=0;
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
    /* A key that declares a buffer the program does not write is refused. */
    key.transform_feedback_buffers=3u;
    out=(void *)1;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)!=VK_SUCCESS && !out);
    /* A capture without a geometry stage has no program. */
    struct ps5vk_graphics_key vertex_only=key;
    vertex_only.geometry=(struct ps5vk_graphics_module_key){0};
    vertex_only.transform_feedback_buffers=1u;
    assert(!ps5vk_runtime_graphics_supported(&vertex_only));
    free_key(&key);

    /* geometryStreams: stream 0 into buffer 0 and stream 1 into buffer 1, one
     * nibble each in the stream/buffer mask. */
    key=capture_key("build/runtime-graphics/xfb_streams.geom.spv",3u);
    assert(ps5vk_spirv_graphics_interface(&key));
    out=NULL;
    assert(ps5vk_runtime_graphics_compile(NULL,&key,&out)==VK_SUCCESS && out);
    p=out;
    assert(p->vertex.metadata.streamout_enabled_stream_buffers_mask==(1u|(2u<<4)));
    assert(p->vertex.metadata.streamout_strides_dwords[0]==4u &&
           p->vertex.metadata.streamout_strides_dwords[1]==4u);
    assert(ps5vk_runtime_streamout_matches(&p->vertex.metadata,3u));
    ps5vk_runtime_graphics_free(NULL,out);
    free_key(&key);
    return 0;
}

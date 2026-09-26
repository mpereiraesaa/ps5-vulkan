#include "vertex_fetch.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static VkResult alloc(void *c,VkDeviceSize n,void **a,void **b)
{(void)c;*a=calloc(1,n);*b=*a;return *a?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;}
static void release(void *c,void *b){(void)c;free(b);}
static VkResult cache(void *c,void *b,VkDeviceSize o,VkDeviceSize n)
{(void)c;(void)b;(void)o;(void)n;return VK_SUCCESS;}
int main(void)
{
    struct VkDevice_T d={.memory={NULL,alloc,release,cache,cache},.buffer_alignment=256,
        .noncoherent_atom=64,.max_allocation=4096};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=240,.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VkBuffer buffer;assert(vkCreateBuffer(&d,&bi,NULL,&buffer)==VK_SUCCESS);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=1024};
    VkDeviceMemory memory;assert(vkAllocateMemory(&d,&mi,NULL,&memory)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,buffer,memory,256)==VK_SUCCESS);
    VkVertexInputBindingDescription binding={0,24,VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32_SFLOAT,12}};
    struct ps5vk_graphics_key key={.vertex_binding_count=1,.vertex_attribute_count=2,
        .vertex_bindings=&binding,.vertex_attributes=attrs};
    struct ps5vk_operation op={.type=PS5VK_DRAW,.vertex_count=3,.instance_count=1,.first_vertex=2};
    op.vertices[0]=(struct ps5vk_vertex_binding){.buffer=buffer,.offset=24};
    uint32_t words[4]={0};assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_SUCCESS);
    void *mapped;assert(vkMapMemory(&d,memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    uint64_t address=(uintptr_t)mapped+256+24;
    assert(words[0]==(uint32_t)address && (words[1]&0xffff)==address>>32 && words[2]==9);
    uint32_t saved[4];memcpy(saved,words,sizeof(words));
    const VkFormat supported[]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R32G32B32A32_SINT,VK_FORMAT_R32_UINT,VK_FORMAT_R32G32_UINT,
        VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32A32_UINT,
        VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_R8_UNORM,VK_FORMAT_R8_SNORM,VK_FORMAT_R8_UINT,VK_FORMAT_R8_SINT,
        VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_R8G8B8A8_UINT,VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        VK_FORMAT_A8B8G8R8_UINT_PACK32,VK_FORMAT_A8B8G8R8_SINT_PACK32,
        VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_UINT,
        VK_FORMAT_R16_SINT,VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16_UINT,
        VK_FORMAT_R16G16_SINT,VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16B16A16_UINT,VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R16G16B16A16_SFLOAT};
    key.vertex_attribute_count=1;
    for(unsigned n=0;n<sizeof(supported)/sizeof(supported[0]);++n) {
        attrs[0].format=supported[n];
        assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_SUCCESS);
    }
    attrs[0].format=VK_FORMAT_R8_USCALED;
    assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_ERROR_FEATURE_NOT_PRESENT);
    attrs[0].format=VK_FORMAT_R8_UNORM;binding.stride=1;op.first_vertex=2;
    op.vertices[0].offset=25;
    struct ps5vk_vertex_fetch fetch={0};
    assert(ps5vk_vertex_fetch_span(&d,&key,&op,&fetch)==VK_SUCCESS &&
        fetch.address==(unsigned char *)mapped+256+25 && fetch.bytes==215 &&
        fetch.stride==1 && fetch.attribute_extent==1);
    assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_ERROR_UNKNOWN);
    op.vertices[0].offset=24;binding.stride=24;
    attrs[0].format=VK_FORMAT_R32G32B32_SFLOAT;key.vertex_attribute_count=2;
    assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_SUCCESS);
    op.first_vertex=7;assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)!=VK_SUCCESS);
    assert(!memcmp(saved,words,sizeof(words)));
    op.first_vertex=UINT32_MAX;assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)!=VK_SUCCESS);
    op.first_vertex=6;assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_SUCCESS);
    op.vertices[0].offset=240;assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)!=VK_SUCCESS);
    op.vertices[0].offset=24;attrs[1].offset=20;assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)!=VK_SUCCESS);
    attrs[1].offset=12;attrs[1].location=32;
    assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)!=VK_SUCCESS);
    attrs[1].location=1;binding.inputRate=VK_VERTEX_INPUT_RATE_INSTANCE;
    assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_ERROR_FEATURE_NOT_PRESENT);
    binding.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;op.vertex_count=0;op.vertices[0].buffer=NULL;
    assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_SUCCESS);
    op.type=PS5VK_DRAW_INDEXED;op.index_count=6;op.vertex_offset=-2;
    op.vertices[0].buffer=buffer;
    assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_SUCCESS && words[2]==9);
    /* A sparse table is indexed by binding number, regardless of description
     * order. One failing buffer must leave the entire output unchanged. */
    VkVertexInputBindingDescription split[2]={{15,12,VK_VERTEX_INPUT_RATE_VERTEX},
                                            {3,24,VK_VERTEX_INPUT_RATE_VERTEX}};
    attrs[0]=(VkVertexInputAttributeDescription){0,3,VK_FORMAT_R32G32B32_SFLOAT,0};
    attrs[1]=(VkVertexInputAttributeDescription){1,15,VK_FORMAT_R32G32B32_SFLOAT,0};
    key.vertex_bindings=split;key.vertex_binding_count=2;
    op.type=PS5VK_DRAW;op.vertex_count=3;op.first_vertex=2;
    op.vertices[3]=(struct ps5vk_vertex_binding){.buffer=buffer,.offset=1};
    op.vertices[15]=(struct ps5vk_vertex_binding){.buffer=buffer,.offset=12};
    struct ps5vk_vertex_fetch_table table={0},snapshot;
    assert(ps5vk_vertex_fetch_spans(&d,&key,&op,&table)==VK_SUCCESS);
    assert(table.count==16 && table.bindings[3].stride==24 && table.bindings[15].stride==12);
    assert(table.bindings[3].address==(unsigned char *)mapped+257);
    assert(table.bindings[15].address==(unsigned char *)mapped+268);
    for(unsigned i=0;i<16;++i)if(i!=3 && i!=15)assert(!table.bindings[i].address);
    struct ps5vk_vertex_fetch_table compact={0};
    assert(ps5vk_vertex_fetch_compact(&table,0x8008,&compact)==VK_SUCCESS && compact.count==2);
    assert(compact.bindings[0].address==table.bindings[3].address);
    assert(compact.bindings[1].address==table.bindings[15].address);
    /* Optimization can drop binding 3: the remaining binding becomes SRD 0. */
    assert(ps5vk_vertex_fetch_compact(&table,0x8000,&compact)==VK_SUCCESS && compact.count==1);
    assert(compact.bindings[0].address==table.bindings[15].address);
    struct ps5vk_vertex_fetch_table compact_saved=compact;
    assert(ps5vk_vertex_fetch_compact(&table,0x8001,&compact)!=VK_SUCCESS);
    assert(ps5vk_vertex_fetch_compact(&table,0x10000,&compact)!=VK_SUCCESS);
    assert(ps5vk_vertex_fetch_compact(&table,0,&compact)!=VK_SUCCESS);
    assert(!memcmp(&compact,&compact_saved,sizeof(compact)));
    op.vertices[3].buffer=NULL;
    assert(ps5vk_vertex_fetch_used_spans(&d,&key,&op,0x8000,&compact)==VK_SUCCESS);
    assert(!compact.bindings[3].address && compact.bindings[15].address);
    op.vertices[3].buffer=buffer;
    snapshot=table;op.vertices[15].offset=235;
    assert(ps5vk_vertex_fetch_spans(&d,&key,&op,&table)!=VK_SUCCESS);
    assert(!memcmp(&table,&snapshot,sizeof(table)));
    op.vertices[15].offset=12;split[0].binding=3;
    assert(ps5vk_vertex_fetch_spans(&d,&key,&op,&table)!=VK_SUCCESS);
    split[0].binding=16;
    assert(ps5vk_vertex_fetch_spans(&d,&key,&op,&table)!=VK_SUCCESS);
    split[0].binding=15;attrs[1].binding=14;
    assert(ps5vk_vertex_fetch_spans(&d,&key,&op,&table)!=VK_SUCCESS);
    attrs[1].binding=15;op.vertex_count=0;op.vertices[3].buffer=NULL;op.vertices[15].buffer=NULL;
    assert(ps5vk_vertex_fetch_spans(&d,&key,&op,&table)==VK_SUCCESS);
    assert(table.count==16);
    for(unsigned i=0;i<16;++i)assert(!table.bindings[i].address);
    VkVertexInputBindingDescription all_bindings[16];
    VkVertexInputAttributeDescription all_attrs[16];
    for(unsigned i=0;i<16;++i) {
        all_bindings[i]=(VkVertexInputBindingDescription){15-i,4,VK_VERTEX_INPUT_RATE_VERTEX};
        all_attrs[i]=(VkVertexInputAttributeDescription){i,i,VK_FORMAT_R32_SFLOAT,0};
        op.vertices[i]=(struct ps5vk_vertex_binding){.buffer=buffer,.offset=i};
    }
    key.vertex_binding_count=key.vertex_attribute_count=16;
    key.vertex_bindings=all_bindings;key.vertex_attributes=all_attrs;op.vertex_count=3;
    assert(ps5vk_vertex_fetch_used_spans(&d,&key,&op,0xffff,&table)==VK_SUCCESS);
    assert(ps5vk_vertex_fetch_compact(&table,0xffff,&compact)==VK_SUCCESS && compact.count==16);
    for(unsigned i=0;i<16;++i)assert(compact.bindings[i].address==(unsigned char *)mapped+256+i);

    /* robustness2. Without the features a null binding and a non-indexed
     * draw past the buffer are refused; with nullDescriptor the null binding
     * keeps its extent and a null address (the all-zero SRD), and with
     * robustBufferAccess2 the SRD's record count bounds the draw instead. */
    key.vertex_binding_count=key.vertex_attribute_count=1;
    key.vertex_bindings=&binding;key.vertex_attributes=attrs;
    binding=(VkVertexInputBindingDescription){0,24,VK_VERTEX_INPUT_RATE_VERTEX};
    attrs[0]=(VkVertexInputAttributeDescription){0,0,VK_FORMAT_R32G32B32_SFLOAT,0};
    op=(struct ps5vk_operation){.type=PS5VK_DRAW,.vertex_count=3,.instance_count=1};
    op.vertices[0]=(struct ps5vk_vertex_binding){.buffer=VK_NULL_HANDLE,.offset=0};
    assert(ps5vk_vertex_fetch_used_spans(&d,&key,&op,1,&table)==VK_ERROR_UNKNOWN);
    d.enabled_features_t09=PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
    assert(ps5vk_vertex_fetch_used_spans(&d,&key,&op,1,&table)==VK_SUCCESS &&
           !table.bindings[0].address && !table.bindings[0].bytes &&
           table.bindings[0].attribute_extent==12 && table.bindings[0].stride==24);
    assert(ps5vk_vertex_fetch_compact(&table,1,&compact)==VK_SUCCESS && compact.count==1 &&
           !compact.bindings[0].address);
    op.vertices[0]=(struct ps5vk_vertex_binding){.buffer=buffer,.offset=24};op.first_vertex=7;
    assert(ps5vk_vertex_fetch_used_spans(&d,&key,&op,1,&table)==VK_ERROR_UNKNOWN);
    d.enabled_features_t09|=PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2;
    assert(ps5vk_vertex_fetch_used_spans(&d,&key,&op,1,&table)==VK_SUCCESS &&
           table.bindings[0].address==(unsigned char *)mapped+256+24 &&
           table.bindings[0].bytes==216);
    d.enabled_features_t09=0;
    vkUnmapMemory(&d,memory);vkDestroyBuffer(&d,buffer,NULL);vkFreeMemory(&d,memory,NULL);
}

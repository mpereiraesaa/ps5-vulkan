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
    op.vertices[0]=(struct ps5vk_vertex_binding){buffer,24};
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
        VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM};
    key.vertex_attribute_count=1;
    for(unsigned n=0;n<sizeof(supported)/sizeof(supported[0]);++n) {
        attrs[0].format=supported[n];
        assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_SUCCESS);
    }
    attrs[0].format=VK_FORMAT_R16_UINT;
    assert(ps5vk_vertex_fetch_descriptor(&d,&key,&op,words)==VK_ERROR_FEATURE_NOT_PRESENT);
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
    vkUnmapMemory(&d,memory);vkDestroyBuffer(&d,buffer,NULL);vkFreeMemory(&d,memory,NULL);
}

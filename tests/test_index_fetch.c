#include "index_fetch.h"
#include <assert.h>
#include <stdlib.h>
static VkResult alloc(void *c,VkDeviceSize n,void **a,void **b)
{(void)c;*a=calloc(1,n);*b=*a;return *a?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;}
static void release(void *c,void *b){(void)c;free(b);}
static VkResult cache(void *c,void *b,VkDeviceSize o,VkDeviceSize n)
{(void)c;(void)b;(void)o;(void)n;return VK_SUCCESS;}
int main(void)
{
    struct VkDevice_T d={.memory={NULL,alloc,release,cache,cache},.buffer_alignment=256,
        .noncoherent_atom=64,.max_allocation=4096};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=240,.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer buffer;assert(vkCreateBuffer(&d,&bi,NULL,&buffer)==VK_SUCCESS);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=1024};
    VkDeviceMemory memory;assert(vkAllocateMemory(&d,&mi,NULL,&memory)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,buffer,memory,256)==VK_SUCCESS);
    void *mapped;assert(vkMapMemory(&d,memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    struct ps5vk_operation op={.type=PS5VK_DRAW_INDEXED,.index_count=6,.instance_count=1,
        .first_index=3,.vertex_offset=-2,.indices={buffer,4,VK_INDEX_TYPE_UINT16}};
    struct ps5vk_index_fetch out={0};
    assert(ps5vk_index_fetch_prepare(&d,&op,&out)==VK_SUCCESS);
    assert(out.address==(uintptr_t)mapped+256+4+6 && out.available_count==115 && out.element_bytes==2);
    op.indices.type=VK_INDEX_TYPE_UINT32;
    assert(ps5vk_index_fetch_prepare(&d,&op,&out)==VK_SUCCESS);
    assert(out.address==(uintptr_t)mapped+256+4+12 && out.available_count==56 && out.element_bytes==4);
    const struct ps5vk_index_fetch saved=out;
    op.first_index=UINT32_MAX;
    assert(ps5vk_index_fetch_prepare(&d,&op,&out)!=VK_SUCCESS && out.address==saved.address);
    op.first_index=53;
    assert(ps5vk_index_fetch_prepare(&d,&op,&out)==VK_SUCCESS && out.available_count==6);
    op.first_index=54;assert(ps5vk_index_fetch_prepare(&d,&op,&out)!=VK_SUCCESS);
    op.first_index=0;op.indices.offset=2;assert(ps5vk_index_fetch_prepare(&d,&op,&out)!=VK_SUCCESS);
    op.indices.offset=240;assert(ps5vk_index_fetch_prepare(&d,&op,&out)!=VK_SUCCESS);
    op.indices.offset=UINT64_MAX;assert(ps5vk_index_fetch_prepare(&d,&op,&out)!=VK_SUCCESS);
    op.indices.offset=0;op.indices.type=VK_INDEX_TYPE_UINT8_EXT;
    assert(ps5vk_index_fetch_prepare(&d,&op,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    op.indices.type=VK_INDEX_TYPE_UINT16;op.indices.buffer=NULL;op.index_count=0;
    assert(ps5vk_index_fetch_prepare(&d,&op,&out)==VK_SUCCESS);
    op.index_count=3;op.instance_count=0;
    assert(ps5vk_index_fetch_prepare(&d,&op,&out)==VK_SUCCESS);
    op.instance_count=1;assert(ps5vk_index_fetch_prepare(&d,&op,&out)!=VK_SUCCESS);
    vkUnmapMemory(&d,memory);vkDestroyBuffer(&d,buffer,NULL);
    bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    assert(vkCreateBuffer(&d,&bi,NULL,&buffer)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,buffer,memory,0)==VK_SUCCESS);
    op.indices.buffer=buffer;assert(ps5vk_index_fetch_prepare(&d,&op,&out)!=VK_SUCCESS);
    vkDestroyBuffer(&d,buffer,NULL);vkFreeMemory(&d,memory,NULL);
}

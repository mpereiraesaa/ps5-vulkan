#include "upload_commands_ps5.h"
#include <assert.h>
#include <string.h>

static _Alignas(64) unsigned char source[128],destination[256];
static unsigned flushes,plans;
static VkResult span_result,plan_result;
VkResult ps5vk_buffer_span(VkDevice d,VkBuffer b,VkDeviceSize offset,VkDeviceSize size,void **p,VkDeviceSize *n)
{
    assert(d && b && offset<sizeof(source));
    *p=source+offset;*n=size==VK_WHOLE_SIZE?sizeof(source)-offset:size;
    return span_result;
}
VkResult ps5vk_image_span(VkDevice d,VkImage image,void **p,VkDeviceSize *n)
{ assert(d && image);*p=destination;*n=sizeof(destination);return span_result; }
VkResult ps5vk_texture_copy_plan_for_image(VkImage image,VkDeviceSize src,VkDeviceSize dst,
    const VkBufferImageCopy *region,struct ps5vk_texture_copy *out)
{
    assert(image && src==sizeof(source) && dst==sizeof(destination) && region);
    ++plans;
    *out=(struct ps5vk_texture_copy){0,0,16,32,8,2,64,96,2};
    return plan_result;
}
static void flush(const void *p,size_t n)
{ assert(p>= (const void *)source && n<=sizeof(source));++flushes; }
int main(void)
{
    struct VkDevice_T device={0};struct VkImage_T image={0};
    VkBuffer buffer=(VkBuffer)(uintptr_t)1; /* Opaque span-stub identity. */
    struct ps5vk_operation ops[4]={
        {.type=PS5VK_BARRIER,.src_stage=VK_PIPELINE_STAGE_HOST_BIT,
         .dst_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,.src_access=VK_ACCESS_HOST_WRITE_BIT,
         .dst_access=VK_ACCESS_TRANSFER_READ_BIT,
         .buffer_barrier={.buffer=buffer,.offset=16,.size=32}},
        {.type=PS5VK_IMAGE_BARRIER,.image_barrier={.image=&image,
         .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT}},
        {.type=PS5VK_COPY_BUFFER_IMAGE,.copy_source=buffer,.copy_image=&image,
         .copy_layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
        {.type=PS5VK_IMAGE_BARRIER,.image_barrier={.image=&image,
         .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT}}
    };
    uint32_t words[256]={0},*cursor=words;struct ps5vk_layout_state layouts={0};
    memset(destination,0xa5,sizeof(destination));
    assert(ps5vk_upload_commands(&device,ops,4,NULL,&layouts,&cursor,words+256,flush)==VK_SUCCESS);
    assert(flushes==2 && plans==1 && cursor-words==3*PS5VK_GRAPHICS_ACQUIRE_WORDS+28);
    /* Real DMA emitter, four rows across two layers, DMA_SYNC on the final row. */
    unsigned start=2*PS5VK_GRAPHICS_ACQUIRE_WORDS;
    assert(words[start]==0xc0055000 && words[start+6]==8);
    assert(!(words[start+1]&0x80000000u) && (words[start+22]&0x80000000u));
    assert(words[start+16]==(uint32_t)(uintptr_t)source+64);
    assert(words[start+18]==(uint32_t)(uintptr_t)destination+96);
    for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0xa5);
    assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED && layouts.count==1);
    assert(ps5vk_layout_require(&layouts,&image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&layouts)==VK_SUCCESS);
    assert(image.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    /* The same prelude may be discarded on any failure without publishing a
     * layout transition or touching image data. */
    image.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    for(unsigned failure=0;failure<4;++failure) {
        layouts=(struct ps5vk_layout_state){0};cursor=words;
        span_result=failure==0?VK_ERROR_MEMORY_MAP_FAILED:VK_SUCCESS;
        plan_result=failure==1?VK_ERROR_UNKNOWN:VK_SUCCESS;
        unsigned count=failure==3?3:4;
        if(failure==3)ops[2].type=PS5VK_DISPATCH;
        assert(ps5vk_upload_commands(&device,ops,count,NULL,&layouts,&cursor,
            words+(failure==2?21:256),flush)!=VK_SUCCESS);
        assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED);
        for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0xa5);
    }
    ops[2].type=PS5VK_COPY_BUFFER_IMAGE;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,ops+2,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    ops[0].dst_access=VK_ACCESS_SHADER_WRITE_BIT;
    assert(ps5vk_upload_commands(&device,ops,1,NULL,&layouts,&cursor,words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT);
}

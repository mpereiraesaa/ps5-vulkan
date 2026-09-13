#include "vk_command.h"
#include "texture_copy.h"
#define invalid ps5vk_command_invalidate
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer c,VkBuffer source,VkImage image,
    VkImageLayout layout,uint32_t count,const VkBufferImageCopy *regions)
{
    if(!c || c->state!=PS5VK_RECORDING || c->render_pass || !count || !regions ||
        count>PS5VK_MAX_OPERATIONS-c->operation_count || !image || image->device!=c->pool->device ||
        (layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && layout!=VK_IMAGE_LAYOUT_GENERAL)) {invalid(c);return;}
    VkDevice d=c->pool->device;
    if(!ps5vk_buffer_usage(d,source,VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        image->info.format!=VK_FORMAT_R8G8B8A8_UNORM || image->info.mipLevels!=1 ||
        !(image->info.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !(image->info.usage&VK_IMAGE_USAGE_SAMPLED_BIT) ||
        (image->info.usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {invalid(c);return;}
    void *src,*dst;VkDeviceSize src_bytes,dst_bytes;
    if(ps5vk_buffer_span(d,source,0,VK_WHOLE_SIZE,&src,&src_bytes)!=VK_SUCCESS ||
        ps5vk_image_span(d,image,&dst,&dst_bytes)!=VK_SUCCESS) {invalid(c);return;}
    /* Validate the whole call before appending any operation. Region structs
     * are copied; later application mutations cannot change recorded work. */
    for(uint32_t i=0;i<count;++i) {
        struct ps5vk_texture_copy plan;
        if(ps5vk_texture_copy_plan(image->info.extent.width,image->info.extent.height,
            src_bytes,dst_bytes,&regions[i],&plan)!=VK_SUCCESS) {invalid(c);return;}
    }
    struct ps5vk_operation *ops=ps5vk_command_reserve_operations(c,PS5VK_COPY_BUFFER_IMAGE,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,count);
    if(!ops)return;
    for(uint32_t i=0;i<count;++i) {
        ops[i].copy_source=source;ops[i].copy_image=image;
        ops[i].copy_layout=layout;ops[i].copy_region=regions[i];
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage image,
    VkImageLayout layout,VkBuffer destination,uint32_t count,const VkBufferImageCopy *regions)
{
    if(!c || c->state!=PS5VK_RECORDING || c->render_pass || count!=1 || !regions ||
        c->operation_count==PS5VK_MAX_OPERATIONS || !image || image->device!=c->pool->device ||
        layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {invalid(c);return;}
    VkDevice d=c->pool->device;
    if(!d->graphics_enabled || !ps5vk_buffer_usage(d,destination,VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        image->info.format!=VK_FORMAT_R8G8B8A8_UNORM || image->info.mipLevels!=1 ||
        image->info.arrayLayers!=1 || image->info.extent.depth!=1 ||
        (image->info.usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))!=
            (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        (image->info.usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {invalid(c);return;}
    const VkBufferImageCopy *r=&regions[0];
    const uint64_t pixels=(uint64_t)image->info.extent.width*image->info.extent.height;
    void *src,*dst;VkDeviceSize src_bytes,dst_bytes;
    if(r->bufferOffset || r->bufferRowLength || r->bufferImageHeight ||
        r->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
        r->imageSubresource.mipLevel || r->imageSubresource.baseArrayLayer ||
        r->imageSubresource.layerCount!=1 || r->imageOffset.x || r->imageOffset.y ||
        r->imageOffset.z || r->imageExtent.width!=image->info.extent.width ||
        r->imageExtent.height!=image->info.extent.height || r->imageExtent.depth!=1 ||
        pixels>UINT64_MAX/4 ||
        ps5vk_image_span(d,image,&src,&src_bytes)!=VK_SUCCESS ||
        ps5vk_buffer_span(d,destination,0,VK_WHOLE_SIZE,&dst,&dst_bytes)!=VK_SUCCESS ||
        dst_bytes<pixels*4) {invalid(c);return;}
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_COPY_IMAGE_BUFFER,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
    if(!op)return;
    op->copy_destination=destination;op->copy_image=image;
    op->copy_layout=layout;op->copy_region=*r;
}

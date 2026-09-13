#include "texture_copy.h"
#include "texture_layout.h"
#include "texture_format.h"
VkResult ps5vk_texture_copy_plan_for_format(VkFormat format,uint32_t width,uint32_t height,VkDeviceSize source_bytes,
    VkDeviceSize destination_bytes,const VkBufferImageCopy *r,struct ps5vk_texture_copy *out)
{
    if(!r || !out)return VK_ERROR_UNKNOWN;
    const struct ps5vk_texture_format *entry=ps5vk_texture_format_lookup(format);
    struct ps5vk_texture_layout layout;
    if(!entry || ps5vk_texture_layout_for_format(format,width,height,&layout) || destination_bytes<layout.bytes ||
        r->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || r->imageSubresource.mipLevel ||
        r->imageSubresource.baseArrayLayer || r->imageSubresource.layerCount!=1 ||
        r->imageOffset.x<0 || r->imageOffset.y<0 || r->imageOffset.z || r->imageExtent.depth!=1 ||
        !r->imageExtent.width || !r->imageExtent.height || r->bufferOffset%entry->bytes_per_texel)return VK_ERROR_UNKNOWN;
    uint32_t x=(uint32_t)r->imageOffset.x,y=(uint32_t)r->imageOffset.y;
    if(x>width || y>height || r->imageExtent.width>width-x || r->imageExtent.height>height-y ||
        (r->bufferRowLength && r->bufferRowLength<r->imageExtent.width) ||
        (r->bufferImageHeight && r->bufferImageHeight<r->imageExtent.height))return VK_ERROR_UNKNOWN;
    uint64_t pitch=(uint64_t)entry->bytes_per_texel*(r->bufferRowLength?r->bufferRowLength:r->imageExtent.width);
    uint64_t row_bytes=(uint64_t)entry->bytes_per_texel*r->imageExtent.width,rows_before=r->imageExtent.height-1;
    if(r->bufferOffset>source_bytes || row_bytes>source_bytes-r->bufferOffset ||
        rows_before>(source_bytes-r->bufferOffset-row_bytes)/pitch)return VK_ERROR_UNKNOWN;
    *out=(struct ps5vk_texture_copy){r->bufferOffset,(uint64_t)y*layout.row_pitch+(uint64_t)entry->bytes_per_texel*x,
        pitch,layout.row_pitch,(uint32_t)row_bytes,r->imageExtent.height};
    return VK_SUCCESS;
}
VkResult ps5vk_texture_copy_plan(uint32_t width,uint32_t height,VkDeviceSize source_bytes,
    VkDeviceSize destination_bytes,const VkBufferImageCopy *r,struct ps5vk_texture_copy *out)
{
    return ps5vk_texture_copy_plan_for_format(VK_FORMAT_R8G8B8A8_UNORM,width,height,
        source_bytes,destination_bytes,r,out);
}

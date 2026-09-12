#include "texture_copy.h"
#include "texture_layout.h"
VkResult ps5vk_texture_copy_plan(uint32_t width,uint32_t height,VkDeviceSize source_bytes,
    VkDeviceSize destination_bytes,const VkBufferImageCopy *r,struct ps5vk_texture_copy *out)
{
    if(!r || !out)return VK_ERROR_UNKNOWN;
    struct ps5vk_texture_layout layout;
    if(ps5vk_texture_layout(width,height,&layout) || destination_bytes<layout.bytes ||
        r->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || r->imageSubresource.mipLevel ||
        r->imageSubresource.baseArrayLayer || r->imageSubresource.layerCount!=1 ||
        r->imageOffset.x<0 || r->imageOffset.y<0 || r->imageOffset.z || r->imageExtent.depth!=1 ||
        !r->imageExtent.width || !r->imageExtent.height || r->bufferOffset%4)return VK_ERROR_UNKNOWN;
    uint32_t x=(uint32_t)r->imageOffset.x,y=(uint32_t)r->imageOffset.y;
    if(x>width || y>height || r->imageExtent.width>width-x || r->imageExtent.height>height-y ||
        (r->bufferRowLength && r->bufferRowLength<r->imageExtent.width) ||
        (r->bufferImageHeight && r->bufferImageHeight<r->imageExtent.height))return VK_ERROR_UNKNOWN;
    uint64_t pitch=4ull*(r->bufferRowLength?r->bufferRowLength:r->imageExtent.width);
    uint64_t row_bytes=4ull*r->imageExtent.width,rows_before=r->imageExtent.height-1;
    if(r->bufferOffset>source_bytes || row_bytes>source_bytes-r->bufferOffset ||
        rows_before>(source_bytes-r->bufferOffset-row_bytes)/pitch)return VK_ERROR_UNKNOWN;
    *out=(struct ps5vk_texture_copy){r->bufferOffset,(uint64_t)y*layout.row_pitch+4ull*x,
        pitch,layout.row_pitch,(uint32_t)row_bytes,r->imageExtent.height};
    return VK_SUCCESS;
}

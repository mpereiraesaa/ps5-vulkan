#include "texture_copy.h"
#include "texture_layout.h"
#include "texture_format.h"
#include "vk_image.h"

static VkResult plan(VkFormat format,uint32_t width,uint32_t height,
    uint32_t image_slices,VkBool32 is_3d,VkDeviceSize source_bytes,
    VkDeviceSize destination_bytes,const VkBufferImageCopy *r,
    struct ps5vk_texture_copy *out)
{
    if(!r || !out)return VK_ERROR_UNKNOWN;
    const struct ps5vk_texture_format *entry=ps5vk_texture_format_lookup(format);
    struct ps5vk_texture_layout layout;
    if(!entry || ps5vk_texture_layout_for_slices(format,width,height,image_slices,&layout) ||
        destination_bytes<layout.bytes ||
        r->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
        r->imageSubresource.mipLevel || !r->imageSubresource.layerCount ||
        r->imageOffset.x<0 || r->imageOffset.y<0 || r->imageOffset.z<0 ||
        !r->imageExtent.width || !r->imageExtent.height || !r->imageExtent.depth ||
        r->bufferOffset%entry->bytes_per_texel)return VK_ERROR_UNKNOWN;
    uint32_t x=(uint32_t)r->imageOffset.x,y=(uint32_t)r->imageOffset.y;
    if(x>width || y>height || r->imageExtent.width>width-x ||
       r->imageExtent.height>height-y ||
       (r->bufferRowLength && r->bufferRowLength<r->imageExtent.width) ||
       (r->bufferImageHeight && r->bufferImageHeight<r->imageExtent.height))
        return VK_ERROR_UNKNOWN;
    uint32_t first_slice=0,slices=0;
    if(is_3d) {
        if(r->imageSubresource.baseArrayLayer || r->imageSubresource.layerCount!=1 ||
           (uint32_t)r->imageOffset.z>image_slices ||
           r->imageExtent.depth>image_slices-(uint32_t)r->imageOffset.z)
            return VK_ERROR_UNKNOWN;
        first_slice=(uint32_t)r->imageOffset.z;slices=r->imageExtent.depth;
    } else {
        if(r->imageOffset.z || r->imageExtent.depth!=1 ||
           r->imageSubresource.baseArrayLayer>=image_slices ||
           r->imageSubresource.layerCount>
                image_slices-r->imageSubresource.baseArrayLayer)
            return VK_ERROR_UNKNOWN;
        first_slice=r->imageSubresource.baseArrayLayer;
        slices=r->imageSubresource.layerCount;
    }
    uint64_t pitch=(uint64_t)entry->bytes_per_texel*
        (r->bufferRowLength?r->bufferRowLength:r->imageExtent.width);
    uint64_t row_bytes=(uint64_t)entry->bytes_per_texel*r->imageExtent.width;
    uint64_t source_slice=pitch*
        (r->bufferImageHeight?r->bufferImageHeight:r->imageExtent.height);
    uint64_t source_span=(uint64_t)(slices-1)*source_slice+
        (uint64_t)(r->imageExtent.height-1)*pitch+row_bytes;
    uint64_t destination_offset=(uint64_t)first_slice*layout.slice_pitch+
        (uint64_t)y*layout.row_pitch+(uint64_t)entry->bytes_per_texel*x;
    uint64_t destination_span=(uint64_t)(slices-1)*layout.slice_pitch+
        (uint64_t)(r->imageExtent.height-1)*layout.row_pitch+row_bytes;
    if(r->bufferOffset>source_bytes || source_span>source_bytes-r->bufferOffset ||
       destination_offset>destination_bytes ||
       destination_span>destination_bytes-destination_offset)
        return VK_ERROR_UNKNOWN;
    *out=(struct ps5vk_texture_copy){r->bufferOffset,destination_offset,pitch,
        layout.row_pitch,(uint32_t)row_bytes,r->imageExtent.height,
        source_slice,layout.slice_pitch,slices};
    return VK_SUCCESS;
}

VkResult ps5vk_texture_copy_plan_for_format(VkFormat format,uint32_t width,
    uint32_t height,VkDeviceSize source_bytes,VkDeviceSize destination_bytes,
    const VkBufferImageCopy *r,struct ps5vk_texture_copy *out)
{
    return plan(format,width,height,1,VK_FALSE,source_bytes,destination_bytes,r,out);
}

VkResult ps5vk_texture_copy_plan_for_image(VkImage image,VkDeviceSize source_bytes,
    VkDeviceSize destination_bytes,const VkBufferImageCopy *r,
    struct ps5vk_texture_copy *out)
{
    if(!image)return VK_ERROR_UNKNOWN;
    const uint32_t slices=image->info.imageType==VK_IMAGE_TYPE_3D?
        image->info.extent.depth:image->info.arrayLayers;
    return plan(image->info.format,image->info.extent.width,image->info.extent.height,
        slices,image->info.imageType==VK_IMAGE_TYPE_3D,
        source_bytes,destination_bytes,r,out);
}

VkResult ps5vk_texture_copy_plan(uint32_t width,uint32_t height,
    VkDeviceSize source_bytes,VkDeviceSize destination_bytes,
    const VkBufferImageCopy *r,struct ps5vk_texture_copy *out)
{
    return ps5vk_texture_copy_plan_for_format(VK_FORMAT_R8G8B8A8_UNORM,
        width,height,source_bytes,destination_bytes,r,out);
}

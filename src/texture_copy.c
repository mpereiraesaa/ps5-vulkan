/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "texture_copy.h"
#include "texture_layout.h"
#include "texture_format.h"
#include "vk_image.h"

/* Overflow-checked arithmetic: a rejected plan must leave the caller's
 * structure untouched, so every quantity is computed into a local first. */
static int mul64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a && b > UINT64_MAX / a) return -1;
    *out = a * b;
    return 0;
}
static int add64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a) return -1;
    *out = a + b;
    return 0;
}

static uint32_t ceil_div32(uint32_t value, uint32_t divisor)
{
    return value / divisor + (value % divisor != 0u);
}

static VkResult plan(VkFormat format,uint32_t width,uint32_t height,
    uint32_t base_slices,uint32_t mip_levels,VkBool32 is_3d,VkDeviceSize source_bytes,
    VkDeviceSize destination_bytes,const VkBufferImageCopy *r,
    struct ps5vk_texture_copy *out)
{
    if(!r || !out)return VK_ERROR_UNKNOWN;
#if PS5VK_D32_SAMPLED_DIAGNOSTIC
    /* The source-derived Dref gather upload is a whole-subresource copy for
     * each level of one 64x64 D32 mip tail. The destination is tiled 64KB_Z_X,
     * so this validates buffer packing only; the queue executor scatters each
     * texel through the tested depth equation instead of using linear rows. */
    if(format==VK_FORMAT_D32_SFLOAT) {
        if(width!=64u || height!=64u || base_slices!=1u || mip_levels!=7u ||
           is_3d || r->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_DEPTH_BIT ||
           r->imageSubresource.mipLevel>=7u || r->imageSubresource.baseArrayLayer ||
           r->imageSubresource.layerCount!=1u || r->imageOffset.x || r->imageOffset.y ||
           r->imageOffset.z || r->bufferOffset%4u) return VK_ERROR_UNKNOWN;
        const uint32_t level=r->imageSubresource.mipLevel;
        const uint32_t extent=width>>level?width>>level:1u;
        if(r->imageExtent.width!=extent || r->imageExtent.height!=extent ||
           r->imageExtent.depth!=1u ||
           (r->bufferRowLength && r->bufferRowLength<extent) ||
           (r->bufferImageHeight && r->bufferImageHeight<extent)) return VK_ERROR_UNKNOWN;
        const uint64_t pitch=(uint64_t)4u*(r->bufferRowLength?r->bufferRowLength:extent);
        const uint64_t span=(uint64_t)(extent-1u)*pitch+(uint64_t)extent*4u;
        if(r->bufferOffset>source_bytes || span>source_bytes-r->bufferOffset ||
           destination_bytes<65536u) return VK_ERROR_UNKNOWN;
        *out=(struct ps5vk_texture_copy){r->bufferOffset,0,pitch,0,
            extent*4u,extent,0,0,1};
        return VK_SUCCESS;
    }
#endif
    /* Formats without an implemented sampled or compressed-block layout have
     * no transfer-copy plan (colour/depth/vertex-only rows stay rejected). */
    if(!ps5vk_texture_format_sampled_encoding(format) &&
       !ps5vk_texture_format_block_compressed(format))return VK_ERROR_UNKNOWN;
    const struct ps5vk_texture_format *entry=ps5vk_texture_format_lookup(format);
    struct ps5vk_texture_mip_layout layout;
    if(!entry || ps5vk_texture_mip_layout_for_slices(format,width,height,base_slices,
            mip_levels,&layout) || r->imageSubresource.mipLevel>=mip_levels ||
        destination_bytes<layout.bytes ||
        r->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
        !r->imageSubresource.layerCount ||
        r->imageOffset.x<0 || r->imageOffset.y<0 || r->imageOffset.z<0 ||
        !r->imageExtent.width || !r->imageExtent.height || !r->imageExtent.depth ||
        !entry->block_width || !entry->block_height || !entry->bytes_per_block ||
        r->bufferOffset%entry->bytes_per_block)return VK_ERROR_UNKNOWN;
    const uint32_t level=r->imageSubresource.mipLevel;
    const uint32_t mip_width=width>>level?width>>level:1;
    const uint32_t mip_height=height>>level?height>>level:1;
    const uint32_t image_slices=is_3d?(base_slices>>level?base_slices>>level:1):base_slices;
    uint32_t x=(uint32_t)r->imageOffset.x,y=(uint32_t)r->imageOffset.y;
    if(x>mip_width || y>mip_height || r->imageExtent.width>mip_width-x ||
       r->imageExtent.height>mip_height-y ||
       (r->bufferRowLength && r->bufferRowLength<r->imageExtent.width) ||
       (r->bufferImageHeight && r->bufferImageHeight<r->imageExtent.height))
        return VK_ERROR_UNKNOWN;
    const uint32_t bw=entry->block_width,bh=entry->block_height;
    const uint32_t row_texels=r->bufferRowLength?r->bufferRowLength:r->imageExtent.width;
    const uint32_t image_texels=r->bufferImageHeight?r->bufferImageHeight:r->imageExtent.height;
    /* Vulkan compressed copies address whole blocks. An offset is block
     * aligned; an extent may end in a partial block only at the image edge.
     * Explicit buffer dimensions still describe whole blocks. */
    if ((x%bw) || (y%bh) ||
        (r->imageExtent.width%bw && x+r->imageExtent.width!=mip_width) ||
        (r->imageExtent.height%bh && y+r->imageExtent.height!=mip_height) ||
        (r->bufferRowLength && r->bufferRowLength%bw) ||
        (r->bufferImageHeight && r->bufferImageHeight%bh))
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
    const uint32_t copy_block_rows=ceil_div32(r->imageExtent.height,bh);
    const uint32_t source_block_columns=ceil_div32(row_texels,bw);
    const uint32_t copy_block_columns=ceil_div32(r->imageExtent.width,bw);
    const uint32_t source_block_rows=ceil_div32(image_texels,bh);
    uint64_t pitch,row_bytes,source_slice,source_span;
    if(mul64(entry->bytes_per_block,source_block_columns,&pitch) ||
       mul64(entry->bytes_per_block,copy_block_columns,&row_bytes) ||
       row_bytes>UINT32_MAX ||
       mul64(pitch,source_block_rows,&source_slice) ||
       mul64(slices-1,source_slice,&source_span) ||
       add64(source_span,(uint64_t)(copy_block_rows-1)*pitch,&source_span) ||
       add64(source_span,row_bytes,&source_span))
        return VK_ERROR_UNKNOWN;
    const struct ps5vk_texture_mip_level *mip=&layout.levels[level];
    uint64_t destination_offset,destination_span;
    if(mul64(first_slice,layout.layer_stride,&destination_offset) ||
       add64(destination_offset,mip->offset,&destination_offset) ||
       add64(destination_offset,(uint64_t)(y/bh)*mip->row_pitch,&destination_offset) ||
       add64(destination_offset,(uint64_t)(x/bw)*entry->bytes_per_block,&destination_offset) ||
       mul64(slices-1,layout.layer_stride,&destination_span) ||
       add64(destination_span,(uint64_t)(copy_block_rows-1)*mip->row_pitch,
             &destination_span) ||
       add64(destination_span,row_bytes,&destination_span))
        return VK_ERROR_UNKNOWN;
    if(r->bufferOffset>source_bytes || source_span>source_bytes-r->bufferOffset ||
       destination_offset>destination_bytes ||
       destination_span>destination_bytes-destination_offset)
        return VK_ERROR_UNKNOWN;
    *out=(struct ps5vk_texture_copy){r->bufferOffset,destination_offset,pitch,
        mip->row_pitch,(uint32_t)row_bytes,copy_block_rows,
        source_slice,layout.layer_stride,slices};
    return VK_SUCCESS;
}

VkResult ps5vk_texture_copy_plan_for_format(VkFormat format,uint32_t width,
    uint32_t height,VkDeviceSize source_bytes,VkDeviceSize destination_bytes,
    const VkBufferImageCopy *r,struct ps5vk_texture_copy *out)
{
    return plan(format,width,height,1,1,VK_FALSE,source_bytes,destination_bytes,r,out);
}

VkResult ps5vk_texture_copy_plan_for_image(VkImage image,VkDeviceSize source_bytes,
    VkDeviceSize destination_bytes,const VkBufferImageCopy *r,
    struct ps5vk_texture_copy *out)
{
    if(!image)return VK_ERROR_UNKNOWN;
    const uint32_t slices=image->info.imageType==VK_IMAGE_TYPE_3D?
        image->info.extent.depth:image->info.arrayLayers;
    return plan(image->info.format,image->info.extent.width,image->info.extent.height,
        slices,image->info.mipLevels,image->info.imageType==VK_IMAGE_TYPE_3D,
        source_bytes,destination_bytes,r,out);
}

VkResult ps5vk_texture_copy_plan(uint32_t width,uint32_t height,
    VkDeviceSize source_bytes,VkDeviceSize destination_bytes,
    const VkBufferImageCopy *r,struct ps5vk_texture_copy *out)
{
    return ps5vk_texture_copy_plan_for_format(VK_FORMAT_R8G8B8A8_UNORM,
        width,height,source_bytes,destination_bytes,r,out);
}

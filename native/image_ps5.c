/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_internal.h"
#include "depth_layout.h"
#include "texture_layout.h"
#include "texture_format.h"
#include "graphics_formats.h"
#include <string.h>

VkResult ps5vk_native_image_requirements(VkDevice d, const VkImageCreateInfo *info,
                                        VkMemoryRequirements *out)
{
    (void)d;
    if (!info || !out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if(!ps5vk_graphics_image_usage(info->format,info->usage))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    int depth = info->format == VK_FORMAT_D32_SFLOAT;
    int sampled = ps5vk_texture_format_sampled_image(info->format);
    int color = info->format == VK_FORMAT_B8G8R8A8_UNORM || info->format == VK_FORMAT_R8G8B8A8_UNORM;
    if ((!depth && !color && !sampled) ||
        info->mipLevels > PS5VK_MAX_TEXTURE_MIP_LEVELS ||
        info->samples != VK_SAMPLE_COUNT_1_BIT || info->tiling != VK_IMAGE_TILING_OPTIMAL)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* A D32 image is the tiled depth surface whether it is used as an
     * attachment, as the destination of a whole-subresource depth clear, or
     * both: there is no linear depth layout for it to fall back to. */
    const int attachment=(info->usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))!=0 || depth;
    const int cube=info->flags==VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if ((info->flags && !cube) ||
        (attachment && (info->flags || info->imageType!=VK_IMAGE_TYPE_2D ||
                        info->arrayLayers!=1 || info->extent.depth!=1)) ||
        (!attachment && info->imageType==VK_IMAGE_TYPE_1D &&
         (cube || info->extent.height!=1 || info->extent.depth!=1 ||
          !info->arrayLayers || info->arrayLayers>PS5VK_MAX_IMAGE_ARRAY_LAYERS ||
          info->extent.width>PS5VK_MAX_IMAGE_1D)) ||
        (!attachment && info->imageType==VK_IMAGE_TYPE_2D &&
         (info->extent.depth!=1 || !info->arrayLayers ||
          info->arrayLayers>PS5VK_MAX_IMAGE_ARRAY_LAYERS ||
          (cube && (info->arrayLayers!=6 ||
                    info->extent.width!=info->extent.height ||
                    info->extent.width>PS5VK_MAX_IMAGE_CUBE)))) ||
        (!attachment && info->imageType==VK_IMAGE_TYPE_3D &&
         (cube || info->arrayLayers!=1 || !info->extent.depth ||
          info->extent.width>PS5VK_MAX_IMAGE_3D ||
          info->extent.height>PS5VK_MAX_IMAGE_3D ||
          info->extent.depth>PS5VK_MAX_IMAGE_3D)) ||
        (!attachment && info->imageType!=VK_IMAGE_TYPE_1D &&
         info->imageType!=VK_IMAGE_TYPE_2D &&
         info->imageType!=VK_IMAGE_TYPE_3D))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* Padded linear layout: the sampled/upload role and the pure transfer role
     * (copy source and/or destination) share one host-visible layout, so the
     * upload path and both transfer directions address the same bytes. */
    if(!depth && (info->usage & (VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
            VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
        !(info->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        struct ps5vk_texture_mip_layout texture;
        const uint32_t slices=info->imageType==VK_IMAGE_TYPE_3D?
            info->extent.depth:info->arrayLayers;
        if(!sampled || (info->mipLevels!=1 && !(info->usage&VK_IMAGE_USAGE_SAMPLED_BIT)) ||
           ps5vk_texture_mip_layout_for_slices(info->format,
            info->extent.width,info->extent.height,slices,info->mipLevels,&texture))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        *out=(VkMemoryRequirements){texture.bytes,texture.alignment,1};return VK_SUCCESS;
    }
    struct ps5vk_depth_layout layout;
    if(info->format==VK_FORMAT_B8G8R8A8_UNORM &&
       (info->extent.width>PS5VK_MAX_COLOR_DIMENSION || info->extent.height>PS5VK_MAX_COLOR_DIMENSION))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (ps5vk_depth_layout(info->extent.width, info->extent.height, &layout))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* One-sample 32-bit 64KB_R_X and 64KB_Z_X have identical 128x128
     * block geometry, NOT identical pixel equations. Reuse footprint arithmetic
     * only. The existing color-target builder requires a 128 KiB base. */
    uint64_t alignment = color ? 131072u : layout.alignment;
    uint64_t size = (layout.bytes + alignment - 1) & ~(alignment - 1);
    *out = (VkMemoryRequirements){size, alignment, 1};
    return VK_SUCCESS;
}

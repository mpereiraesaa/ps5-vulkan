/*
 * Copyright (C) 2026 Manuel Pereira
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFX10.3 texture resource types, dimension fields and the SW_64K_R_X
 * render-target sampling encoding adapted from
 * BlackBearReloaded's ps5-opengl, src/gallium/ps5/ps5_screen.c at commit
 * 7f9bfabdddb187a11e4401058eba8c9e55194d0a (GPL-3.0-or-later).
 */
#include "texture_descriptor.h"
#include "texture_layout.h"
#include "texture_format.h"
#include <string.h>
/* The GFX10 image fields both encoders share, and nothing else. The sampled
 * entry adds its own usage rules before calling this and its sampler words
 * after; the resource-only entry adds the input-attachment contract. Nothing
 * here reads a sampler, and a failure leaves out untouched. */
static VkResult image_resource_words(VkDevice d,VkImageView view,uint32_t out[8])
{
    VkImage image=view->image;
    const struct ps5vk_texture_format *format=ps5vk_texture_format_lookup(view->format);
    if(!format || !ps5vk_texture_format_sampled_image(view->format) || image->info.format!=view->format ||
        view->range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || !view->range.levelCount ||
        view->range.baseMipLevel>=image->info.mipLevels ||
        view->range.levelCount>image->info.mipLevels-view->range.baseMipLevel ||
        !view->range.layerCount || image->info.mipLevels>PS5VK_MAX_TEXTURE_MIP_LEVELS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t slices=image->info.imageType==VK_IMAGE_TYPE_3D?
        image->info.extent.depth:image->info.arrayLayers;
    if(!slices || view->range.baseArrayLayer>=
       (image->info.imageType==VK_IMAGE_TYPE_3D?1u:image->info.arrayLayers) ||
       view->range.layerCount>(image->info.imageType==VK_IMAGE_TYPE_3D?1u:image->info.arrayLayers)-
            view->range.baseArrayLayer)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_texture_mip_layout layout;
    if(ps5vk_texture_mip_layout_for_slices(view->format,image->info.extent.width,
        image->info.extent.height,slices,image->info.mipLevels,&layout))return VK_ERROR_UNKNOWN;
    void *base;VkDeviceSize bytes;
    VkResult rc=ps5vk_image_span(d,image,&base,&bytes);if(rc!=VK_SUCCESS)return rc;
    uint64_t address=(uintptr_t)base,limit=UINT64_C(1)<<48;
    if(!address || address>=limit || address%layout.alignment ||
       bytes<layout.bytes || layout.bytes>limit-address)return VK_ERROR_UNKNOWN;
    uint32_t type_word=0,dimension_word=0;
    switch(view->view_type) {
    case VK_IMAGE_VIEW_TYPE_1D:
        if(image->info.imageType!=VK_IMAGE_TYPE_1D || view->range.layerCount!=1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if(layout.layer_stride>limit-address ||
           view->range.baseArrayLayer>(limit-address)/layout.layer_stride)
            return VK_ERROR_UNKNOWN;
        address+=layout.layer_stride*view->range.baseArrayLayer;
        type_word=8u<<28;
        break;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        if(image->info.imageType!=VK_IMAGE_TYPE_1D)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        type_word=12u<<28;
        dimension_word=(view->range.baseArrayLayer<<16)|
            (view->range.baseArrayLayer+view->range.layerCount-1);
        break;
    case VK_IMAGE_VIEW_TYPE_2D:
        if(image->info.imageType!=VK_IMAGE_TYPE_2D || view->range.layerCount!=1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if(layout.layer_stride>limit-address ||
           view->range.baseArrayLayer>(limit-address)/layout.layer_stride)
            return VK_ERROR_UNKNOWN;
        address+=layout.layer_stride*view->range.baseArrayLayer;
        type_word=9u<<28;
        if(image->info.mipLevels==1 &&
           layout.levels[0].row_pitch/format->bytes_per_texel!=image->info.extent.width)
            dimension_word=layout.levels[0].row_pitch/format->bytes_per_texel-1;
        break;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        if(image->info.imageType!=VK_IMAGE_TYPE_2D)return VK_ERROR_FEATURE_NOT_PRESENT;
        type_word=13u<<28;
        dimension_word=(view->range.baseArrayLayer<<16)|
            (view->range.baseArrayLayer+view->range.layerCount-1);
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
        if(image->info.imageType!=VK_IMAGE_TYPE_2D ||
           !(image->info.flags&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
           view->range.baseArrayLayer || view->range.layerCount!=6)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        type_word=11u<<28; /* one cube: descriptor word 4 stores cube count - 1 */
        break;
    case VK_IMAGE_VIEW_TYPE_3D:
        if(image->info.imageType!=VK_IMAGE_TYPE_3D ||
           view->range.baseArrayLayer || view->range.layerCount!=1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        type_word=10u<<28;dimension_word=image->info.extent.depth-1;break;
    default:return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* Public Mesa gfx10-rsrc.json descriptor fields plus the pinned
     * ps5-opengl format and identity-swizzle mapping. */
    uint32_t words[8]={0},width=image->info.extent.width-1;
    words[0]=(uint32_t)(address>>8);
    words[1]=(uint32_t)(address>>40)|format->descriptor_format_word|((width&3u)<<30);
    words[2]=(width>>2)|((image->info.extent.height-1)<<14)|(1u<<31);
    words[3]=ps5vk_texture_format_dst_sel(format)|type_word|
        (view->range.baseMipLevel<<12)|
        ((view->range.baseMipLevel+view->range.levelCount-1)<<16);
    words[4]=dimension_word;
    words[5]=(4u<<20)|((image->info.mipLevels-1)<<4);
    memcpy(out,words,sizeof(words));return VK_SUCCESS;
}

VkResult ps5vk_texture_descriptor(VkDevice d,VkImageView view,VkSampler sampler,uint32_t out[12])
{
    if(!d || !view || !sampler || !out || view->device!=d || sampler->device!=d || !view->image)return VK_ERROR_UNKNOWN;
    const VkImageUsageFlags usage=view->image->info.usage;
    if(!(usage&VK_IMAGE_USAGE_SAMPLED_BIT) ||
       (usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t words[12];
    VkResult rc=image_resource_words(d,view,words);
    if(rc!=VK_SUCCESS)return rc;
    memcpy(words+8,sampler->words,16);memcpy(out,words,sizeof(words));return VK_SUCCESS;
}

VkResult ps5vk_image_resource_descriptor(VkDevice d,VkImageView view,uint32_t out[8])
{
    if(!d || !view || !out || view->device!=d || !view->image)return VK_ERROR_UNKNOWN;
    const VkImage image=view->image;
    if(image->device!=d)return VK_ERROR_UNKNOWN;
    /* Exactly the resource contract this profile publishes and witnessed: the
     * RGBA8 attachment shape, created for input-attachment use, viewed as a 2D
     * or 2D_ARRAY image. Anything else - another format, a sampled-only image,
     * a deeper or multisampled image, a 3D/1D/cube view - fails closed rather
     * than being encoded as something the GPU was never witnessed to read. */
    if(view->format!=VK_FORMAT_R8G8B8A8_UNORM || image->info.format!=view->format ||
        image->info.imageType!=VK_IMAGE_TYPE_2D || image->info.extent.depth!=1u ||
        image->info.mipLevels!=1u || image->info.samples!=VK_SAMPLE_COUNT_1_BIT ||
        !image->info.arrayLayers ||
        image->info.arrayLayers>(uint32_t)PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR ||
        !(image->info.usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ||
        !(view->view_type==VK_IMAGE_VIEW_TYPE_2D || view->view_type==VK_IMAGE_VIEW_TYPE_2D_ARRAY))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t words[8];
    VkResult rc=image_resource_words(d,view,words);
    if(rc!=VK_SUCCESS)return rc;
    /* This resource is also the live colour attachment. Its backing is the
     * target builder's SW_64K_R_X surface, not the padded-linear sampled-image
     * layout used by ordinary uploaded textures. The pinned GPL reference uses
     * 0x91b.../0xd1b... for 2D/2D-array render targets; the 0x01b00000 part is
     * the common tiled-layout encoding and the type, selectors and dimensions
     * already present in words[] remain unchanged. Encoding the linear form
     * here addresses valid memory with the wrong equation and collapses a
     * subpassLoad to an unrelated constant texel on hardware. */
    words[3]|=UINT32_C(0x01b00000);
    memcpy(out,words,sizeof(words));return VK_SUCCESS;
}

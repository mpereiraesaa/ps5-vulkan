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
#include "depth_layout.h"
#include <string.h>

/* The GFX10 image fields both encoders share, and nothing else. The sampled
 * entry adds its own usage rules before calling this and its sampler words
 * after; the resource-only entry adds the input-attachment contract. Nothing
 * here reads a sampler, and a failure leaves out untouched. */
static VkResult image_resource_words(VkDevice d,VkImageView view,uint32_t out[8])
{
    VkImage image=view->image;
    const struct ps5vk_texture_format *format=ps5vk_texture_format_lookup(view->format);
    const VkBool32 d32_gather=ps5vk_d32_gather_image(image);
    /* A mutable image's view may name an implemented reinterpretation of the
     * image format: same texel block and layout, so only the data-format field
     * of word 1 below differs (src/texture_format.c). */
    const VkBool32 view_format_ok=image->info.format==view->format ||
        (image->mutable_format &&
         ps5vk_texture_format_view_compatible(image->info.format,view->format));
    if(!format || !ps5vk_texture_format_sampled_image(view->format) || !view_format_ok ||
        (view->range.aspectMask!=(d32_gather?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT)) || !view->range.levelCount ||
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
    VkDeviceSize layer_stride=layout.layer_stride;
    const VkBool32 tiled_cube=ps5vk_tiled_cube_sampled_image(image);
    if(tiled_cube) {
        if(image->requirements.size%image->info.arrayLayers)
            return VK_ERROR_UNKNOWN;
        layer_stride=image->requirements.size/image->info.arrayLayers;
        /* The sampler derives its array pitch from the tiled footprint. A
         * smaller attachment layer padded to the target's 128 KiB alignment
         * would silently sample each face twice. Refuse that descriptor. */
        struct ps5vk_depth_layout tiled;
        if(ps5vk_depth_layout(image->info.extent.width,
            image->info.extent.height,&tiled) || tiled.bytes!=layer_stride)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
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
        if(layer_stride>limit-address ||
           view->range.baseArrayLayer>(limit-address)/layer_stride)
            return VK_ERROR_UNKNOWN;
        address+=layer_stride*view->range.baseArrayLayer;
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
        if(layer_stride>limit-address ||
           view->range.baseArrayLayer>(limit-address)/layer_stride)
            return VK_ERROR_UNKNOWN;
        address+=layer_stride*view->range.baseArrayLayer;
        type_word=9u<<28;
        /* GFX10 sampled-resource word 4 carries DEPTH/BASE_ARRAY state; it
         * has no MIP0_WIDTH pitch field. The render-target MIP0_WIDTH register
         * is a different descriptor. A 2D view's depth is one, including when
         * linear upload rows are padded or stored in compressed blocks. */
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
           view->range.layerCount!=6 ||
           image->info.extent.width!=image->info.extent.height)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        type_word=11u<<28;
        dimension_word=(view->range.baseArrayLayer<<16)|
            (view->range.baseArrayLayer+view->range.layerCount-1);
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        if(!(d->enabled_features&PS5VK_FEATURE_IMAGE_CUBE_ARRAY) ||
           image->info.imageType!=VK_IMAGE_TYPE_2D ||
           !(image->info.flags&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
           view->range.layerCount%6 ||
           image->info.extent.width!=image->info.extent.height)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        type_word=11u<<28;
        dimension_word=(view->range.baseArrayLayer<<16)|
            (view->range.baseArrayLayer+view->range.layerCount-1);
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
    if(tiled_cube)words[3]|=UINT32_C(0x01b00000);
    if(d32_gather) words[3]|=24u<<20; /* GFX10 64KB_Z_X depth mip tail */
    words[4]=dimension_word;
    words[5]=(4u<<20)|((image->info.mipLevels-1)<<4);
    memcpy(out,words,sizeof(words));return VK_SUCCESS;
}

VkResult ps5vk_texture_descriptor(VkDevice d,VkImageView view,VkSampler sampler,uint32_t out[12])
{
    if(!d || !view || !sampler || !out || view->device!=d || sampler->device!=d || !view->image)return VK_ERROR_UNKNOWN;
    const VkImageUsageFlags usage=view->image->info.usage;
    if(!(usage&VK_IMAGE_USAGE_SAMPLED_BIT) ||
       ((usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
        !ps5vk_tiled_cube_sampled_image(view->image)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if(ps5vk_d32_gather_image(view->image)!=sampler->compare_enable)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if(ps5vk_d32_gather_image(view->image) &&
       (!view->range.levelCount || view->range.baseMipLevel ||
        view->range.levelCount!=7u || view->range.aspectMask!=VK_IMAGE_ASPECT_DEPTH_BIT))
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
    /* A multisampled colour attachment is readable through the same record
     * (DXVK262-T06): the pinned gfx6+ texture descriptor carries the sample
     * geometry in the LEVEL fields - BASE_LEVEL 0 and LAST_LEVEL
     * log2(samples) for a multisampled surface
     * (ac_descriptors.c ac_build_gfx6_texture_descriptor) - so a multisampled
     * surface is single-layer, single-mip and otherwise the same attachment.
     * A count this profile does not implement stays refused here. */
    const uint32_t samples=ps5vk_sample_count_number(image->info.samples);
    const int multisampled=samples>1u;
    if(view->format!=VK_FORMAT_R8G8B8A8_UNORM || image->info.format!=view->format ||
        image->info.imageType!=VK_IMAGE_TYPE_2D || image->info.extent.depth!=1u ||
        image->info.mipLevels!=1u ||
        (!multisampled && image->info.samples!=VK_SAMPLE_COUNT_1_BIT) ||
        (multisampled && image->info.arrayLayers!=1u) ||
        !image->info.arrayLayers ||
        image->info.arrayLayers>(uint32_t)PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR ||
        /* The INPUT_ATTACHMENT role is what an APPLICATION's descriptor read
         * requires, and the input-attachment gate is where that shape is
         * enforced for app-recorded sets. The driver's own resolve draw reads
         * the multisampled colour attachment of the subpass it resolves, and a
         * subpass resolve is implementation work rather than an app read: the
         * pinned CTS creates exactly that image with COLOR_ATTACHMENT and
         * TRANSFER_SRC alone (RENDER_TYPE_RESOLVE,
         * vktPipelineMultisampleTests.cpp), so a multisampled record needs the
         * colour-attachment role the pass itself promises instead - measured:
         * without this, every min_sample_shading triangle leaf failed at
         * vkQueueSubmit with VK_ERROR_FEATURE_NOT_PRESENT from the resolve
         * emission's descriptor build. */
        (!multisampled && !(image->info.usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) ||
        (multisampled && !(image->info.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) ||
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
    if(multisampled) {
        /* The resource TYPE has to say the surface is multisampled - the pinned
         * compiler's own type tag is V_008F1C_SQ_RSRC_IMG_2D_MSAA (14) rather
         * than the plain 2D tag (9) - and BASE_LEVEL [12,15] stays zero while
         * LAST_LEVEL [16,19] names the sample count, which is how the same
         * compiler describes a multisampled texture to this hardware
         * (ac_descriptors.c, ac_build_gfx6_texture_descriptor). A record with
         * the plain 2D tag and no sample geometry reads the surface as
         * single-sample data - measured: every sample index returned plane
         * zero's value. */
        words[3]&=(uint32_t)~((UINT32_C(0xf)<<28)|UINT32_C(0x000ff000));
        words[3]|=(UINT32_C(15)<<28)|(ps5vk_sample_count_log2(image->info.samples)<<16);
        /* The pinned compiler lowers a subpassInputMS to a TWO DIMENSIONAL
         * ARRAY MSAA image (ac_shader_util.c: GLSL_SAMPLER_DIM_SUBPASS_MS ->
         * ac_image_2darraymsaa), so the resource type tag is
         * V_008F1C_SQ_RSRC_IMG_2D_MSAA_ARRAY (15) - not the plain 2D_MSAA tag
         * (14) - and the single layer this profile serves is described with a
         * zero depth field. The plain subpassInput the multiview witness reads
         * is the same story with the non-MSAA array tag, which is why that
         * record already carries 13. */
        words[4]=0;
        /* The pinned compiler's multisampled surface descriptor ALSO names the
         * sample geometry in the MAX_MIP field (ac_descriptors.c,
         * ac_build_gfx6_texture_descriptor: desc[5] MAX_MIP(log2(num_samples))
         * on the GFX9 path). The GFX10 path in the same function does not write
         * it, but this profile's hardware is reached through AGC rather than
         * through that builder, so the field is carried here too: measured
         * separately, and recorded as measured. */
        words[5]&=(uint32_t)~UINT32_C(0x000000f0);
        words[5]|=(ps5vk_sample_count_log2(image->info.samples)<<4);
    }
    memcpy(out,words,sizeof(words));return VK_SUCCESS;
}

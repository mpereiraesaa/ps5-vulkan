/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_internal.h"
#include "depth_layout.h"
#include "texture_layout.h"
#include "texture_format.h"
#include "graphics_formats.h"
#include "color_attachment_contract.h"
#include <string.h>

/* Storage of one array layer of an attachment surface, and of a whole layered
 * attachment. The per-layer quantity is the footprint the single-layer path has
 * always used, and the one slice A measured by pointing the target registers at
 * a second slot: color targets are 128 KiB-aligned, D32 uses the 64KB_Z_X
 * layout's own alignment (64 KiB). A layer stride is that footprint, so layer N
 * begins at N * stride and every layer starts on an address the target builders
 * accept. `layers` is multiplied with an explicit overflow check, and
 * `layers == 1` reproduces the single-layer requirements exactly. */
VkResult ps5vk_native_layered_storage(VkFormat format, uint32_t width, uint32_t height,
    uint64_t layers, VkDeviceSize *stride, VkDeviceSize *alignment, VkDeviceSize *bytes)
{
    return ps5vk_native_layered_storage_samples(format, width, height, layers,
        VK_SAMPLE_COUNT_1_BIT, stride, alignment, bytes);
}

VkResult ps5vk_native_layered_storage_samples(VkFormat format, uint32_t width, uint32_t height,
    uint64_t layers, VkSampleCountFlagBits samples, VkDeviceSize *stride,
    VkDeviceSize *alignment, VkDeviceSize *bytes)
{
    if (!stride || !alignment || !bytes || !layers) return VK_ERROR_UNKNOWN;
    /* The layer count is 64-bit so the multiplication below is the only thing
     * standing between a caller and a wrapped size. */
    *stride = 0; *alignment = 0; *bytes = 0;
    /* The colour-target storage equation is shared by every format this build
     * renders into, including the integer target the independentBlend
     * measurement serves. */
    const int color = ps5vk_color_target_format_supported(format);
    const int depth = format == VK_FORMAT_D32_SFLOAT;
    if ((!color && !depth) || !width || !height) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* A multisampled surface stores one plane per sample, so the layer's bytes
     * scale with the count (pinned: ac_estimate_size multiplies each level's
     * bytes by num_samples). Only the colour role carries more than one sample
     * on this path; the depth role has no multisampled target yet, and a count
     * this profile does not implement is refused here as everywhere else. */
    const uint32_t sample_count = ps5vk_sample_count_number(samples);
    if (!sample_count || (sample_count > 1 && !color)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (color && (width > PS5VK_MAX_COLOR_DIMENSION || height > PS5VK_MAX_COLOR_DIMENSION))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    struct ps5vk_depth_layout layout;
    if (ps5vk_depth_layout(width, height, &layout)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* One-sample 32-bit 64KB_R_X and 64KB_Z_X share footprint arithmetic but
     * not pixel equations; the existing color-target builder requires a 128 KiB
     * base, the depth builder its layout alignment. */
    const uint64_t base_alignment = color ? 131072u : layout.alignment;
    if (!base_alignment || (base_alignment & (base_alignment - 1u)) ||
        layout.bytes > UINT64_MAX - (base_alignment - 1u))
        return VK_ERROR_UNKNOWN;
    if (layout.bytes > UINT64_MAX / sample_count) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const uint64_t sampled_bytes = layout.bytes * sample_count;
    const uint64_t layer_bytes = (sampled_bytes + base_alignment - 1u) & ~(base_alignment - 1u);
    if (!layer_bytes || layer_bytes > UINT64_MAX / layers) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *stride = layer_bytes;
    *alignment = base_alignment;
    *bytes = layer_bytes * layers;
    return VK_SUCCESS;
}

VkResult ps5vk_native_image_requirements(VkDevice d, const VkImageCreateInfo *info,
                                        VkMemoryRequirements *out)
{
    if (!info || !out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    /* The pinned upstream draw module's host-readback staging image is the one
     * linear-tiling image this profile backs. Its bytes are the same padded
     * linear layout the transfer role and the upload path address, so the
     * requirements are that layout's, not the tiled colour surface's. Anything
     * else that asks for linear tiling is refused here as well as in
     * vkCreateImage. */
    if (info->tiling == VK_IMAGE_TILING_LINEAR) {
        struct ps5vk_texture_layout layout;
        if (info->format != VK_FORMAT_R8G8B8A8_UNORM ||
            info->imageType != VK_IMAGE_TYPE_2D ||
            info->mipLevels != 1 || info->arrayLayers != 1 ||
            info->samples != VK_SAMPLE_COUNT_1_BIT ||
            info->extent.depth != 1 || info->flags ||
            info->usage != VK_IMAGE_USAGE_TRANSFER_DST_BIT ||
            ps5vk_texture_layout_for_format(info->format, info->extent.width,
                info->extent.height, &layout))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        *out = (VkMemoryRequirements){layout.bytes, layout.alignment, 1};
        return VK_SUCCESS;
    }
    int depth = info->format == VK_FORMAT_D32_SFLOAT;
    int sampled = ps5vk_texture_format_sampled_image(info->format);
    if ((info->usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
        (info->format != VK_FORMAT_R32_UINT || info->imageType != VK_IMAGE_TYPE_2D ||
         info->extent.width > 8 || info->extent.height > 8 ||
         info->mipLevels != 1 || info->arrayLayers != 1 || info->samples != VK_SAMPLE_COUNT_1_BIT ||
         info->flags || info->usage != (VK_IMAGE_USAGE_STORAGE_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* The colour-target footprint (one 64KB_R_X surface, 128 KiB-aligned) is
     * shared by every format this build renders into, including the integer
     * target the independentBlend measurement serves. */
    int color = ps5vk_color_target_format_supported(info->format);
    /* The sample counts follow the platform mask and the colour contract
     * (DXVK262-T06): every other role this profile backs is single-sample, and
     * the colour attachment takes the served 2x/4x counts only on a platform
     * that carries the feature bit, as a 2D one-mip image created with exactly
     * the role combinations the pinned multisample oracle builds. That one
     * shape's usage set is not in the format table's combinations, because
     * those describe single-sample images, so the role is checked here and the
     * generic combination check is skipped for it. A count whose multisampled
     * storage nothing backs is refused here as well as in vkCreateImage. */
    const uint32_t sample_count = ps5vk_sample_count_number(info->samples);
    const int multisampled_color =
        sample_count > 1 && color && !depth &&
        (info->format == VK_FORMAT_B8G8R8A8_UNORM ||
         info->format == VK_FORMAT_R8G8B8A8_UNORM) &&
        d && info->imageType == VK_IMAGE_TYPE_2D && info->mipLevels == 1 &&
        !info->flags && ps5vk_multisampled_color_usage(info->usage) &&
        (ps5vk_platform_sample_counts(d->platform_features) & info->samples) != 0;
    if (!multisampled_color && !ps5vk_graphics_image_usage_with_flags(
            info->format,info->imageType,info->tiling,info->usage,info->flags))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!sample_count || (sample_count > 1 && !multisampled_color))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((!depth && !color && !sampled &&
         !(info->format == VK_FORMAT_R32_UINT &&
           (info->usage & VK_IMAGE_USAGE_STORAGE_BIT))) ||
        info->mipLevels > PS5VK_MAX_TEXTURE_MIP_LEVELS ||
        info->tiling != VK_IMAGE_TILING_OPTIMAL)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* A D32 image is the tiled depth surface whether it is used as an
     * attachment, as the destination of a whole-subresource depth clear, or
     * both: there is no linear depth layout for it to fall back to. */
    const int attachment=(info->usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))!=0 || depth;
    const int cube=info->flags==VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if ((info->flags && !cube) ||
        (cube && (info->imageType!=VK_IMAGE_TYPE_2D || info->arrayLayers<6 ||
                  info->extent.width!=info->extent.height ||
                  info->extent.width>PS5VK_MAX_IMAGE_CUBE)) ||
        (attachment && (info->imageType!=VK_IMAGE_TYPE_2D ||
                        !info->arrayLayers ||
                        info->arrayLayers>PS5VK_MAX_IMAGE_ARRAY_LAYERS ||
                        info->extent.depth!=1)) ||
        (!attachment && info->imageType==VK_IMAGE_TYPE_1D &&
         (cube || info->extent.height!=1 || info->extent.depth!=1 ||
          !info->arrayLayers || info->arrayLayers>PS5VK_MAX_IMAGE_ARRAY_LAYERS ||
          info->extent.width>PS5VK_MAX_IMAGE_1D)) ||
        (!attachment && info->imageType==VK_IMAGE_TYPE_2D &&
         (info->extent.depth!=1 || !info->arrayLayers ||
          info->arrayLayers>PS5VK_MAX_IMAGE_ARRAY_LAYERS)) ||
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
    /* Layered attachments: one footprint per array layer, so the same storage
     * model slice A measured now describes an array target. arrayLayers == 1
     * reproduces the previous requirements byte for byte. */
    VkDeviceSize stride = 0, alignment = 0, size = 0;
    VkResult layered = ps5vk_native_layered_storage_samples(info->format, info->extent.width,
        info->extent.height, info->arrayLayers, info->samples, &stride, &alignment, &size);
    if (layered != VK_SUCCESS) return layered;
    *out = (VkMemoryRequirements){size, alignment, 1};
    return VK_SUCCESS;
}

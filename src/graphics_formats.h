/*
 * Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Vertex numeric categories follow the PSBC/Gallium contract in the pinned
 * GPL ps5-opengl reference; ps5-vulkan owns the Vulkan-facing validation.
 */
#ifndef PS5VK_GRAPHICS_FORMATS_H
#define PS5VK_GRAPHICS_FORMATS_H
#include <vulkan/vulkan_core.h>
#include "graphics_limits.h"
#include "texture_format.h"

enum ps5vk_vertex_numeric {
    PS5VK_VERTEX_NUMERIC_NONE = 0,
    PS5VK_VERTEX_NUMERIC_FLOAT,
    PS5VK_VERTEX_NUMERIC_SINT,
    PS5VK_VERTEX_NUMERIC_UINT,
};
struct ps5vk_vertex_format {
    uint32_t bytes, components;
    enum ps5vk_vertex_numeric numeric;
};

/* Vertex format metadata is shared by capability queries, SPIR-V interface
 * matching, the PSBC adapter and the bounded fetch descriptor.  The 32-bit
 * integer rows follow the PSBC/Gallium mapping used by the pinned GPL
 * ps5-opengl implementation. The 8/16-bit families are exposed only together
 * with the pinned PSBC lowering and ps5-vulkan's conversion oracles. */
static inline struct ps5vk_vertex_format ps5vk_vertex_format_info(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R32_SFLOAT: return (struct ps5vk_vertex_format){4,1,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R32G32_SFLOAT: return (struct ps5vk_vertex_format){8,2,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R32G32B32_SFLOAT: return (struct ps5vk_vertex_format){12,3,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R32G32B32A32_SFLOAT: return (struct ps5vk_vertex_format){16,4,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R32_SINT: return (struct ps5vk_vertex_format){4,1,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R32G32_SINT: return (struct ps5vk_vertex_format){8,2,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R32G32B32_SINT: return (struct ps5vk_vertex_format){12,3,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R32G32B32A32_SINT: return (struct ps5vk_vertex_format){16,4,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R32_UINT: return (struct ps5vk_vertex_format){4,1,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R32G32_UINT: return (struct ps5vk_vertex_format){8,2,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R32G32B32_UINT: return (struct ps5vk_vertex_format){12,3,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R32G32B32A32_UINT: return (struct ps5vk_vertex_format){16,4,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
        return (struct ps5vk_vertex_format){1,1,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R8_UINT:
        return (struct ps5vk_vertex_format){1,1,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R8_SINT:
        return (struct ps5vk_vertex_format){1,1,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
        return (struct ps5vk_vertex_format){2,2,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R8G8_UINT:
        return (struct ps5vk_vertex_format){2,2,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R8G8_SINT:
        return (struct ps5vk_vertex_format){2,2,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return (struct ps5vk_vertex_format){4,4,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32:
        return (struct ps5vk_vertex_format){4,4,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32:
        return (struct ps5vk_vertex_format){4,4,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_SFLOAT:
        return (struct ps5vk_vertex_format){2,1,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R16_UINT:
        return (struct ps5vk_vertex_format){2,1,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R16_SINT:
        return (struct ps5vk_vertex_format){2,1,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_SFLOAT:
        return (struct ps5vk_vertex_format){4,2,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R16G16_UINT:
        return (struct ps5vk_vertex_format){4,2,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R16G16_SINT:
        return (struct ps5vk_vertex_format){4,2,PS5VK_VERTEX_NUMERIC_SINT};
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return (struct ps5vk_vertex_format){8,4,PS5VK_VERTEX_NUMERIC_FLOAT};
    case VK_FORMAT_R16G16B16A16_UINT:
        return (struct ps5vk_vertex_format){8,4,PS5VK_VERTEX_NUMERIC_UINT};
    case VK_FORMAT_R16G16B16A16_SINT:
        return (struct ps5vk_vertex_format){8,4,PS5VK_VERTEX_NUMERIC_SINT};
    default: return (struct ps5vk_vertex_format){0};
    }
}
static inline uint32_t ps5vk_vertex_format_size(VkFormat format)
{ return ps5vk_vertex_format_info(format).bytes; }

/* Native executable image roles. RGBA8 is a bounded off-screen color target
 * with transfer-source readback, a sampled/upload target, and a pure
 * transfer role; BGRA8 remains the VideoOut target. */
static inline int ps5vk_graphics_image_usage(VkFormat format, VkImageUsageFlags usage)
{
    switch(format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    case VK_FORMAT_D32_SFLOAT:
        return usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    case VK_FORMAT_R8G8B8A8_UNORM:
        /* Pure transfer role: the padded linear layout used by the upload path,
         * which also serves vkCmdCopyImage, vkCmdClearColorImage and the
         * buffer transfers without pretending the tiled colour-attachment
         * layout is linear. The role is host-visible memory that no GPU stage
         * touches, so a copy source alone is a real (if write-less) role. */
        if (usage && !(usage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
            return 1;
        /* Sampled role: the same padded linear layout, uploaded by the GPU
         * prelude and read by shader descriptors. */
        return (usage &&
            !(usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT))) ||
            /* Tiled colour-attachment role. VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
             * is reported for this format, and a reported format feature implies
             * an image with that usage can be created, so the bare attachment
             * usage must be accepted as well as the readback pair. */
            usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ||
            usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    default:
        return ps5vk_texture_format_supported(format) && usage &&
            !(usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    }
}

/* Implementation ceilings from the encoders/layout arithmetic, not a hardware
 * certification at maximum dimensions. The resource-size budget also applies. */
static inline VkResult ps5vk_graphics_image_properties(VkFormat format,
    VkImageType type,VkImageTiling tiling,VkImageUsageFlags usage,
    VkImageCreateFlags flags,VkDeviceSize budget,VkImageFormatProperties *out)
{
    *out=(VkImageFormatProperties){0};
    if(tiling!=VK_IMAGE_TILING_OPTIMAL || !budget ||
       !ps5vk_graphics_image_usage(format,usage))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const VkBool32 attachment=(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))!=0;
    const VkBool32 sampled=ps5vk_texture_format_supported(format) &&
        (usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT))!=0 &&
        !attachment;
    const VkBool32 transfer_only=format==VK_FORMAT_R8G8B8A8_UNORM && usage &&
        !(usage&~(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    uint32_t width=0,height=0,depth=1,layers=1;
    if(attachment) {
        if(type!=VK_IMAGE_TYPE_2D || flags)return VK_ERROR_FORMAT_NOT_SUPPORTED;
        width=height=format==VK_FORMAT_B8G8R8A8_UNORM?
            PS5VK_MAX_COLOR_DIMENSION:PS5VK_MAX_IMAGE_2D;
    } else if(transfer_only && type==VK_IMAGE_TYPE_2D && !flags) {
        width=height=PS5VK_MAX_IMAGE_2D;
    } else if(sampled && type==VK_IMAGE_TYPE_2D && !flags) {
        width=height=PS5VK_MAX_IMAGE_2D;layers=PS5VK_MAX_IMAGE_ARRAY_LAYERS;
    } else if(sampled && type==VK_IMAGE_TYPE_2D &&
              flags==VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
        width=height=PS5VK_MAX_IMAGE_CUBE;layers=6;
    } else if(sampled && type==VK_IMAGE_TYPE_3D && !flags) {
        width=height=depth=PS5VK_MAX_IMAGE_3D;
    } else return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out=(VkImageFormatProperties){.maxExtent={width,height,depth},
        .maxMipLevels=1,.maxArrayLayers=layers,.sampleCounts=VK_SAMPLE_COUNT_1_BIT,
        .maxResourceSize=budget};
    return VK_SUCCESS;
}

/* Bounded execution profile, not all states accepted by host object creation.
 * In particular: no BGRA sampling or depth sampling. */
static inline void ps5vk_graphics_format_properties(VkFormat format,
                                                    VkFormatProperties *out)
{
    *out = (VkFormatProperties){0};
    if (ps5vk_vertex_format_size(format))
        out->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT ||
        format == VK_FORMAT_R32_SFLOAT)
        out->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    const struct ps5vk_texture_format *sampled=ps5vk_texture_format_lookup(format);
    if(sampled && ps5vk_texture_format_supported(format)) {
        out->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if(sampled->linear_filter_validated)
            out->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    }
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
        /* TRANSFER_DST is real for the transfer-only role implemented by the
         * image-copy/clear slice (padded linear layout), not a claim about the
         * tiled colour-attachment layout. */
        out->optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        break;
    case VK_FORMAT_D32_SFLOAT:
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        break;
    default: break;
    }
}
#endif

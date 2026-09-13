#ifndef PS5VK_GRAPHICS_FORMATS_H
#define PS5VK_GRAPHICS_FORMATS_H
#include <vulkan/vulkan_core.h>
#include "graphics_limits.h"

/* Float attribute widths shared by capability queries and the fetch gate. */
static inline uint32_t ps5vk_vertex_format_size(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32_SFLOAT: return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

/* Native executable image roles. RGBA8 is a bounded off-screen color target
 * with transfer-source readback; BGRA8 remains the VideoOut target. */
static inline int ps5vk_graphics_image_usage(VkFormat format, VkImageUsageFlags usage)
{
    switch(format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    case VK_FORMAT_D32_SFLOAT:
        return usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return (usage &&
            !(usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT))) ||
            usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    default: return 0;
    }
}

/* Implementation ceilings from the encoders/layout arithmetic, not a hardware
 * certification at maximum dimensions. The resource-size budget also applies. */
static inline VkResult ps5vk_graphics_image_properties(VkFormat format,
    VkImageType type,VkImageTiling tiling,VkImageUsageFlags usage,
    VkImageCreateFlags flags,VkDeviceSize budget,VkImageFormatProperties *out)
{
    *out=(VkImageFormatProperties){0};
    if(type!=VK_IMAGE_TYPE_2D || tiling!=VK_IMAGE_TILING_OPTIMAL || flags ||
       !budget || !ps5vk_graphics_image_usage(format,usage))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint32_t dimension=format==VK_FORMAT_B8G8R8A8_UNORM?PS5VK_MAX_COLOR_DIMENSION:PS5VK_MAX_IMAGE_2D;
    *out=(VkImageFormatProperties){.maxExtent={dimension,dimension,1},
        .maxMipLevels=1,.maxArrayLayers=1,.sampleCounts=VK_SAMPLE_COUNT_1_BIT,
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
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        break;
    case VK_FORMAT_D32_SFLOAT:
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        break;
    default: break;
    }
}
#endif

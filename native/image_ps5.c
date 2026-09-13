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
    int sampled = ps5vk_texture_format_supported(info->format);
    int color = info->format == VK_FORMAT_B8G8R8A8_UNORM || info->format == VK_FORMAT_R8G8B8A8_UNORM;
    if ((!depth && !color && !sampled) || info->imageType != VK_IMAGE_TYPE_2D ||
        info->mipLevels != 1 || info->arrayLayers != 1 || info->extent.depth != 1 ||
        info->samples != VK_SAMPLE_COUNT_1_BIT || info->tiling != VK_IMAGE_TILING_OPTIMAL)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* Padded linear layout: the sampled/upload role and the pure transfer role
     * (copy source and/or destination) share one host-visible layout, so the
     * upload path and both transfer directions address the same bytes. */
    if((info->usage & (VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
            VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
        !(info->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        struct ps5vk_texture_layout texture;
        if(!sampled || ps5vk_texture_layout_for_format(info->format,
            info->extent.width,info->extent.height,&texture))return VK_ERROR_FORMAT_NOT_SUPPORTED;
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

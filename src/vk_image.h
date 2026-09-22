#ifndef PS5VK_IMAGE_H
#define PS5VK_IMAGE_H
#include "vk_internal.h"
#include "color_attachment_contract.h"
struct VkImage_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator, ever_bound;
    VkImageCreateInfo info;
    VkMemoryRequirements requirements;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    unsigned pending, views;
    /* Committed only after confirmed completion; calloc initializes UNDEFINED. */
    VkImageLayout layout;
    /* Native display ownership is independent of queued rendering references. */
    VkBool32 display_busy;
    struct VkImage_T *next;
};
struct VkImageView_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkImage image;
    VkImageViewType view_type;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned pending, framebuffers;
};
VkResult ps5vk_image_span(VkDevice, VkImage, void **address, VkDeviceSize *bytes);
/* The host-visible padded-linear transfer role: RGBA8, one mip/layer/sample,
 * optimal tiling, usage drawn from the two transfer bits only. No GPU stage can
 * sample, render into or read such an image, so its transfers and layout
 * transitions are frontend work over the same padded layout the upload path
 * uses. */
/* The colour-attachment readback shape that also declares a transfer
 * destination; the clear and buffer-upload destination paths accept it. */
/* The descriptor shape of the one linear image this profile creates:
 * RGBA8, 2D, one mip, one layer, one sample, LINEAR tiling, usage TRANSFER_DST
 * alone. Used by vkCreateImage to accept it and to refuse every other linear
 * combination before an object exists. */
/* The pinned upstream draw module's host-readback staging image itself: the
 * linear-tiling image vkGetImageSubresourceLayout may describe and the only
 * destination a colour-attachment readback copy may name. */
static inline int ps5vk_linear_staging_descriptor(const VkImageCreateInfo *info)
{
    return info && info->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO &&
        info->tiling == VK_IMAGE_TILING_LINEAR && !info->pNext && !info->flags &&
        info->format == VK_FORMAT_R8G8B8A8_UNORM &&
        info->imageType == VK_IMAGE_TYPE_2D &&
        info->mipLevels == 1 && info->arrayLayers == 1 &&
        info->samples == VK_SAMPLE_COUNT_1_BIT && info->extent.depth == 1 &&
        info->usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT &&
        info->sharingMode == VK_SHARING_MODE_EXCLUSIVE &&
        info->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED;
}
static inline VkBool32 ps5vk_pure_transfer_image(VkImage image)
{
    if (!image) return VK_FALSE;
    return image->info.format == VK_FORMAT_R8G8B8A8_UNORM &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 1 &&
        image->info.extent.depth == 1 && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL && image->info.usage &&
        !(image->info.usage &
          ~(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
}
static inline VkBool32 ps5vk_linear_staging_image(VkImage image)
{
    if (!image) return VK_FALSE;
    return image->info.tiling == VK_IMAGE_TILING_LINEAR &&
        image->info.format == VK_FORMAT_R8G8B8A8_UNORM &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 1 &&
        image->info.extent.depth == 1 &&
        image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}
static inline VkBool32 ps5vk_colour_transfer_image(VkImage image)
{
    if (!image) return VK_FALSE;
    /* The normalized readback target this profile has always carried, and -
     * only in the build that serves it - the integer target its
     * independentBlend oracle reads back. The upstream render-pass module adds
     * the sampled role to that same image because the format publishes it, so
     * the extra bit is tolerated exactly where that combination is served. */
    const VkImageUsageFlags extra = ps5vk_color_sampled_readback_served() ?
        (VkImageUsageFlags)VK_IMAGE_USAGE_SAMPLED_BIT : 0u;
    return (image->info.format == VK_FORMAT_R8G8B8A8_UNORM ||
            ps5vk_color_target_integer_served(image->info.format)) &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 1 &&
        image->info.extent.depth == 1 && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        (image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        (image->info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        !(image->info.usage &
          ~(VkImageUsageFlags)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra));
}

/* The only layered colour source the linear readback path accepts.  It is the
 * promoted input-attachment backing itself, and a readback always addresses
 * layer zero through a one-layer VkImageCopy.  Keeping this separate from the
 * ordinary one-layer colour-transfer role prevents INPUT_ATTACHMENT (or six
 * layers) from widening any clear/upload path by accident. */
static inline VkBool32 ps5vk_input_attachment_readback_image(VkImage image)
{
    const VkImageUsageFlags exact = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image) return VK_FALSE;
    return image->info.format == VK_FORMAT_R8G8B8A8_UNORM &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 6 &&
        image->info.extent.depth == 1 &&
        image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        image->info.usage == exact && !image->info.flags;
}

/* The depth surface an upstream case renders into and then reads back: a
 * single-sample, single-layer D32 attachment that also declares the transfer
 * source role, and optionally the whole-subresource clear destination. Kept
 * to exactly that shape because it is the only one whose SW_64K_Z_X pixel
 * addressing is implemented (src/depth_detile.c). */
static inline VkBool32 ps5vk_depth_readback_image(VkImage image)
{
    if (!image) return VK_FALSE;
    return image->info.format == VK_FORMAT_D32_SFLOAT &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 1 &&
        image->info.extent.depth == 1 && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        (image->info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
        (image->info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        !(image->info.usage &
          ~(VkImageUsageFlags)(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT));
}

static inline VkBool32 ps5vk_colour_readback_image(VkImage image)
{
    return ps5vk_colour_transfer_image(image) ||
        ps5vk_input_attachment_readback_image(image);
}
#endif

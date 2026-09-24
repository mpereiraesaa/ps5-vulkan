#ifndef PS5VK_IMAGE_H
#define PS5VK_IMAGE_H
#include "vk_internal.h"
#include "color_attachment_contract.h"
#include "texture_format.h"
#include "texture_layout.h"
#ifndef PS5VK_RGBA8_INTEGER_ATTACHMENT_DIAGNOSTIC
#define PS5VK_RGBA8_INTEGER_ATTACHMENT_DIAGNOSTIC 0
#endif
#ifndef PS5VK_D32_SAMPLED_DIAGNOSTIC
#define PS5VK_D32_SAMPLED_DIAGNOSTIC 0
#endif
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
    /* Optional mip-major committed layouts. MAX_ENUM marks a mixed image. */
    VkImageLayout *subresource_layouts;
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
/* Resolve counts by subtraction, so oversized ranges cannot wrap. */
static inline int ps5vk_image_range_resolve(VkImage image,
    const VkImageSubresourceRange *range, VkImageSubresourceRange *out)
{
    if (!image || !range || !out || range->baseMipLevel >= image->info.mipLevels ||
        range->baseArrayLayer >= image->info.arrayLayers) return 0;
    *out = *range;
    const uint32_t mips = image->info.mipLevels - range->baseMipLevel;
    const uint32_t layers = image->info.arrayLayers - range->baseArrayLayer;
    if (out->levelCount == VK_REMAINING_MIP_LEVELS) out->levelCount = mips;
    if (out->layerCount == VK_REMAINING_ARRAY_LAYERS) out->layerCount = layers;
    return out->levelCount && out->levelCount <= mips &&
        out->layerCount && out->layerCount <= layers;
}
static inline int ps5vk_image_layout_matches(VkImage image, uint32_t mip,
    uint32_t base_layer, uint32_t layer_count, VkImageLayout expected)
{
    if (!image || mip >= image->info.mipLevels || base_layer >= image->info.arrayLayers ||
        !layer_count || layer_count > image->info.arrayLayers - base_layer) return 0;
    if (!image->subresource_layouts) return image->layout == expected;
    for (uint32_t i = 0; i < layer_count; ++i)
        if (image->subresource_layouts[(size_t)mip * image->info.arrayLayers + base_layer + i]
            != expected) return 0;
    return 1;
}
static inline int ps5vk_image_range_layout_matches(VkImage image,
    const VkImageSubresourceRange *range, VkImageLayout expected)
{
    VkImageSubresourceRange r;
    if (!ps5vk_image_range_resolve(image, range, &r)) return 0;
    for (uint32_t m = 0; m < r.levelCount; ++m)
        if (!ps5vk_image_layout_matches(image, r.baseMipLevel + m,
            r.baseArrayLayer, r.layerCount, expected)) return 0;
    return 1;
}
static inline int ps5vk_image_layout_transition(VkImage image,
    const VkImageSubresourceRange *range, VkImageLayout old, VkImageLayout next)
{
    VkImageSubresourceRange r;
    if (!ps5vk_image_range_resolve(image, range, &r)) return 0;
    if (!image->subresource_layouts && (r.baseMipLevel || r.baseArrayLayer ||
        r.levelCount != image->info.mipLevels || r.layerCount != image->info.arrayLayers)) return 0;
    for (uint32_t m = 0; old != VK_IMAGE_LAYOUT_UNDEFINED && m < r.levelCount; ++m)
        if (!ps5vk_image_layout_matches(image, r.baseMipLevel + m,
            r.baseArrayLayer, r.layerCount, old)) return 0;
    if (image->subresource_layouts) {
        for (uint32_t m = 0; m < r.levelCount; ++m)
            for (uint32_t l = 0; l < r.layerCount; ++l)
                image->subresource_layouts[(size_t)(r.baseMipLevel + m) *
                    image->info.arrayLayers + r.baseArrayLayer + l] = next;
        next = image->subresource_layouts[0];
        const size_t count = (size_t)image->info.mipLevels * image->info.arrayLayers;
        for (size_t i = 1; i < count; ++i)
            if (image->subresource_layouts[i] != next) { next = VK_IMAGE_LAYOUT_MAX_ENUM; break; }
    }
    image->layout = next;
    return 1;
}
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
    return (image->info.format == VK_FORMAT_R8G8B8A8_UNORM ||
            image->info.format == VK_FORMAT_R8G8B8A8_SRGB) &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 1 &&
        image->info.extent.depth == 1 && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL && image->info.usage &&
        !(image->info.usage &
          ~(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
}
static inline VkBool32 ps5vk_rgba_linear_image(VkImage image)
{
    if (!image) return VK_FALSE;
    const VkImageCreateInfo *i = &image->info;
    const VkImageUsageFlags allowed = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return (i->format == VK_FORMAT_R8G8B8A8_UNORM || i->format == VK_FORMAT_R8G8B8A8_SRGB) &&
        i->imageType == VK_IMAGE_TYPE_2D && i->tiling == VK_IMAGE_TILING_OPTIMAL &&
        i->extent.depth == 1 && i->mipLevels && i->mipLevels <= PS5VK_MAX_TEXTURE_MIP_LEVELS &&
        i->arrayLayers && i->samples == VK_SAMPLE_COUNT_1_BIT &&
        (i->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
        !(i->usage & ~allowed) &&
        (!(i->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
         (i->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT));
}
/* Bounded host-linear executor role for block-compressed transfer images.
 * These images use block-padded mip storage and must never enter the RGBA8
 * byte-per-texel row copier. */
static inline VkBool32 ps5vk_bc_linear_image(VkImage image)
{
    if (!image) return VK_FALSE;
    const VkImageCreateInfo *i = &image->info;
    const VkImageUsageFlags allowed = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return ps5vk_texture_format_block_compressed(i->format) &&
        i->imageType == VK_IMAGE_TYPE_2D && i->tiling == VK_IMAGE_TILING_OPTIMAL &&
        i->extent.depth == 1 && i->mipLevels && i->mipLevels <= PS5VK_MAX_TEXTURE_MIP_LEVELS &&
        i->arrayLayers &&
        i->samples == VK_SAMPLE_COUNT_1_BIT &&
        (i->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
        !(i->usage & ~allowed);
}
/* Linear transfer/sampling layouts share the same cache visibility contract. */
static inline int ps5vk_linear_layout_access(VkImage image, VkImageLayout layout,
    VkAccessFlags access, int destination)
{
    VkAccessFlags allowed = 0;
    const VkImageUsageFlags usage = image->info.usage;
    /* Access scopes describe ordered operations, not the accesses performed
     * in this layout. In particular the upstream upload helper orders WRITE
     * before READ while retaining TRANSFER_DST_OPTIMAL. Validate supported
     * usage and stage scopes separately from the layout's usage requirement. */
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) allowed |= VK_ACCESS_TRANSFER_READ_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) allowed |= VK_ACCESS_TRANSFER_WRITE_BIT;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) allowed |= VK_ACCESS_SHADER_READ_BIT;
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return !destination && !access;
    case VK_IMAGE_LAYOUT_GENERAL: break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        if (!(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return 0;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        if (!(usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        if (!(usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return 0;
        break;
    default: return 0;
    }
    return !(access & ~allowed);
}
static inline int ps5vk_linear_image_barrier(const VkImageMemoryBarrier *b)
{
    VkImageSubresourceRange r;
    return b && ps5vk_image_range_resolve(b->image, &b->subresourceRange, &r) &&
        r.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
        ps5vk_linear_layout_access(b->image, b->oldLayout, b->srcAccessMask, 0) &&
        ps5vk_linear_layout_access(b->image, b->newLayout, b->dstAccessMask, 1);
}
/* R32_UINT UAV with the same padded row layout as a one-level texture. */
static inline VkBool32 ps5vk_storage_image(VkImage image)
{
    return image && image->info.format == VK_FORMAT_R32_UINT &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 1 &&
        image->info.extent.depth == 1 && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        image->info.usage == (VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
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

static inline VkBool32 ps5vk_basic_colour_readback_image(VkImage image)
{
    const VkImageUsageFlags exact = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return image && image->info.format == VK_FORMAT_R8G8B8A8_UNORM &&
        image->info.imageType == VK_IMAGE_TYPE_2D && image->info.mipLevels == 1 &&
        image->info.arrayLayers == 1 && image->info.extent.depth == 1 &&
        image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        image->info.usage == exact && !image->info.flags;
}

static inline VkBool32 ps5vk_integer_colour_readback_image(VkImage image)
{
    const VkImageUsageFlags exact = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return image &&
        (image->info.format == VK_FORMAT_R8G8B8A8_UINT ||
         (PS5VK_RGBA8_INTEGER_ATTACHMENT_DIAGNOSTIC &&
          image->info.format == VK_FORMAT_R8G8B8A8_SINT)) &&
        image->info.imageType == VK_IMAGE_TYPE_2D && image->info.mipLevels == 1 &&
        image->info.arrayLayers == 1 && image->info.extent.depth == 1 &&
        image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        image->info.usage == exact && !image->info.flags;
}

static inline VkBool32 ps5vk_d32_gather_image(VkImage image)
{
    const VkImageUsageFlags exact = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return PS5VK_D32_SAMPLED_DIAGNOSTIC && image &&
        image->info.format == VK_FORMAT_D32_SFLOAT &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.extent.width == 64 && image->info.extent.height == 64 &&
        image->info.extent.depth == 1 && image->info.mipLevels == 7 &&
        image->info.arrayLayers == 1 && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL && !image->info.flags &&
        image->info.usage == exact;
}

static inline VkBool32 ps5vk_colour_readback_image(VkImage image)
{
    return ps5vk_basic_colour_readback_image(image) ||
        ps5vk_integer_colour_readback_image(image) ||
        ps5vk_colour_transfer_image(image) ||
        ps5vk_input_attachment_readback_image(image);
}
#endif

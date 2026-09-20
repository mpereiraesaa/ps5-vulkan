#ifndef PS5VK_COLOR_BARRIER_H
#define PS5VK_COLOR_BARRIER_H
#include "vk_image.h"

/* Initial discard transition. Access masks select a scope, not a prescribed
 * READ|WRITE pair. The frontend separately checks stage/access compatibility,
 * ownership and the complete subresource range. */
static inline int ps5vk_color_discard_barrier(const VkImageMemoryBarrier *b)
{
    const VkAccessFlags allowed=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT |
        VK_ACCESS_MEMORY_WRITE_BIT;
    return b && b->image && (b->image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
        b->newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        !b->srcAccessMask && !(b->dstAccessMask & ~allowed);
}
/* Return a completed readback target to rendering. The queue orders jobs and
 * the native prelude acquires caches; layout tracking still checks oldLayout.
 * This is not a discard: preserve image contents until the next render pass. */
static inline int ps5vk_color_readback_reuse_barrier(const VkImageMemoryBarrier *b)
{
    const VkImageUsageFlags required=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return b && b->image && (b->image->info.usage & required)==required &&
        b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        b->newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        b->srcAccessMask==VK_ACCESS_TRANSFER_READ_BIT &&
        b->dstAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
}
#endif

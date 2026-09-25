#ifndef PS5VK_READBACK_REGION_H
#define PS5VK_READBACK_REGION_H
#include "vk_image.h"
#include <stdint.h>

/* DXVK262-T10: one region of a general colour readback (vkCmdCopyImageToBuffer
 * of a 32-bit colour image). Shared by the recorder, which refuses a region
 * that does not satisfy it, and by the native readback, which re-derives the
 * same bytes before it detiles. */
/* The buffer bytes one region writes, from its first texel to one past its
 * last, or 0 when the region is malformed for this image. */
static inline uint64_t ps5vk_readback_region_bytes(VkImage image, const VkBufferImageCopy *r)
{
    const VkImageCreateInfo *i = &image->info;
    const uint64_t row = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width;
    const uint64_t height = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
    if (r->imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
        r->imageSubresource.mipLevel || !r->imageSubresource.layerCount ||
        r->imageSubresource.baseArrayLayer >= i->arrayLayers ||
        r->imageSubresource.layerCount > i->arrayLayers - r->imageSubresource.baseArrayLayer ||
        r->imageOffset.x < 0 || r->imageOffset.y < 0 || r->imageOffset.z ||
        !r->imageExtent.width || !r->imageExtent.height || r->imageExtent.depth != 1 ||
        (uint64_t)(uint32_t)r->imageOffset.x + r->imageExtent.width > i->extent.width ||
        (uint64_t)(uint32_t)r->imageOffset.y + r->imageExtent.height > i->extent.height ||
        row < r->imageExtent.width || height < r->imageExtent.height ||
        (r->bufferOffset & 3u))
        return 0;
    const uint64_t layer = row * height * 4u;
    return layer * (r->imageSubresource.layerCount - 1u) +
        (uint64_t)(r->imageExtent.height - 1u) * row * 4u + (uint64_t)r->imageExtent.width * 4u;
}

#endif

#ifndef PS5VK_COLOR_BARRIER_H
#define PS5VK_COLOR_BARRIER_H
#include "vk_image.h"

/* The destination scope the pinned upstream render-pass module gives the
 * transition that initializes an attachment it is about to clear.
 *
 * The module does not name the transfer write alone: it names every memory
 * read the later drawing and reading of that attachment can perform, plus the
 * write the clear itself performs
 * (vktRenderPassTests.cpp:457 getAllMemoryReadFlags() |
 * VK_ACCESS_TRANSFER_WRITE_BIT, recorded by pushImageInitializationCommands at
 * vktRenderPassTests.cpp:3150). An access mask selects a scope, not a
 * prescribed operation, so the extra read bits neither authorize the clear to
 * read the image nor add a dependency the transition did not have; they widen
 * which later accesses are ordered by it. The mask is spelled out here once so
 * the barrier profiles bound themselves to it instead of accepting any
 * access set, and so a change to it is a single reviewed edit. */
static inline VkAccessFlags ps5vk_attachment_initialization_read_mask(void)
{
    return VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
}

/* The companion write scope, getAllMemoryWriteFlags() in the same module
 * (vktRenderPassTests.cpp:466). It is what the module names as the source of
 * the barrier that hands a finished attachment to its readback, after its own
 * render pass has already left that attachment in its final layout. */
static inline VkAccessFlags ps5vk_attachment_initialization_write_mask(void)
{
    return VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
}

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

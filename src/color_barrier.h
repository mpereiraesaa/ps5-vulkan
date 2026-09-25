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

/* Handing a rendered colour attachment to its readback. The render-pass
 * module's own pass already leaves the attachment in its final layout, the
 * transfer-source one, so this barrier is the identity and all it does is order
 * the writes the pass performed against the copy's read
 * (pushReadImagesToBuffers, vktRenderPassTests.cpp:3582: the source is every
 * memory write plus the layout's own access, the destination is every memory
 * read). The identity is admitted only with a write on the source side and the
 * copy's own read on the destination side, so an empty or foreign scope is
 * still refused. The recorded image barrier profile and the native readback
 * validator both ask this one predicate rather than spelling it twice. */
static inline int ps5vk_colour_readback_handover_barrier(const VkImageMemoryBarrier *b)
{
    return b &&
        b->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        (b->srcAccessMask & ps5vk_attachment_initialization_write_mask()) &&
        !(b->srcAccessMask &
          ~(ps5vk_attachment_initialization_write_mask() |
            (VkAccessFlags)VK_ACCESS_TRANSFER_READ_BIT)) &&
        (b->dstAccessMask & VK_ACCESS_TRANSFER_READ_BIT) &&
        !(b->dstAccessMask & ~ps5vk_attachment_initialization_read_mask());
}

/* The pair the pinned render-pass module records around a clear it performs
 * itself, before the pass that will render into the cleared attachment: the
 * acquire that discards the old contents into the transfer-destination layout
 * with the module's whole read scope plus the write the clear performs, and the
 * handover that gives the cleared attachment to its attachment layout with the
 * module's read scope plus the access that layout's owner writes with. The
 * recorder accepts exactly these two, and the native executor agrees with the
 * same two predicates rather than spelling them again. */
static inline int ps5vk_attachment_initialization_acquire_barrier(const VkImageMemoryBarrier *b)
{
    return b && b->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && !b->srcAccessMask &&
        (b->dstAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) &&
        !(b->dstAccessMask &
          ~(ps5vk_attachment_initialization_read_mask() |
            (VkAccessFlags)VK_ACCESS_TRANSFER_WRITE_BIT));
}
static inline int ps5vk_attachment_initialization_handover_barrier(const VkImageMemoryBarrier *b)
{
    return b && b->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        b->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        b->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT &&
        (b->dstAccessMask & (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT)) &&
        /* DXVK262-T10: a D3D11 runtime hands its cleared render target over
         * with the image's whole access scope, which also names the transfer
         * write a later copy into it performs (DXVK: 0x1980). */
        !(b->dstAccessMask &
          ~(ps5vk_attachment_initialization_read_mask() |
            (VkAccessFlags)(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)));
}
/* DXVK262-T10: the readback hand-over and hand-back a D3D11 runtime records
 * with srcAccessMask 0, because a preceding global memory barrier already made
 * the attachment writes (or the copy's reads) available (DXVK 2.6.2:
 * COLOR_ATTACHMENT -> TRANSFER_SRC dst TRANSFER_READ, and TRANSFER_SRC ->
 * COLOR_ATTACHMENT dst COLOR_READ|COLOR_WRITE|TRANSFER_READ|TRANSFER_WRITE).
 * Only the layout moves; the destination scope stays bounded to what the
 * next use performs. */
static inline int ps5vk_colour_readback_dependency_barrier(const VkImageMemoryBarrier *b)
{
    const VkAccessFlags handback = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT;
    return b && b->image && !b->srcAccessMask &&
        (b->image->info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        (b->image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        ((b->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
          b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
          b->dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT) ||
         (b->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
          b->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
          (b->dstAccessMask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) &&
          !(b->dstAccessMask & ~handback)));
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

/* The original precise-occlusion case renders into a 128x128 RGBA8 target in
 * GENERAL, then reads it back. Keep its two transitions tied to that exact
 * attachment role and to the stages the pinned test records. Both recorder
 * and native prelude use this predicate so a recorded barrier is executable. */
static inline int ps5vk_precise_query_colour_barrier(const VkImageMemoryBarrier *b,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    if (!b || !ps5vk_basic_colour_readback_image(b->image) ||
        b->image->info.extent.width != 128u ||
        b->image->info.extent.height != 128u) return 0;
    return (b->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            b->newLayout == VK_IMAGE_LAYOUT_GENERAL &&
            !b->srcAccessMask &&
            b->dstAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
            src_stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT &&
            dst_stage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) ||
           (b->oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
            b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            b->srcAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
            b->dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT &&
            src_stage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
            dst_stage == VK_PIPELINE_STAGE_TRANSFER_BIT);
}
#endif

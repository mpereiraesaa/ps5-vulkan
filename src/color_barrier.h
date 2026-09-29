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
/* The first x64 PE DXVK backbuffer use is a colour-output scoped discard,
 * not a TOP_OF_PIPE acquire. Keep the recorder and native prelude on this
 * exact mutable BGRA8 four-role image transition. */
static inline int ps5vk_dxvk_bgra8_initial_color_barrier(const VkImageMemoryBarrier *b,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    return b && ps5vk_tiled_2d_sampled_color_image(b->image) &&
        b->image->info.format == VK_FORMAT_B8G8R8A8_UNORM &&
        ps5vk_color_discard_barrier(b) &&
        b->dstAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
        src_stage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
        dst_stage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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

/* A BGRA8 attachment's exact handover to a full-surface buffer readback.
 * The copy preserves its BGRA byte order; no format conversion is implied. */
static inline int ps5vk_bgra8_readback_barrier(const VkImageMemoryBarrier *b,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    return b && ps5vk_bgra8_colour_readback_image(b->image) &&
        b->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        b->srcAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
        b->dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT &&
        src_stage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
        dst_stage == VK_PIPELINE_STAGE_TRANSFER_BIT;
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
/* DXVK 2.6.2's tiled colour backbuffer is rendered, sampled and copied in
 * separate submissions. The recorder and native upload prelude must accept
 * the same measured layout transitions, or submit fails after record succeeds.
 * The queue serializes jobs and the prelude performs a conservative acquire;
 * the empty source scopes here follow DXVK's preceding global dependency. */
static inline int ps5vk_dxvk_tiled_backbuffer_barrier(const VkImageMemoryBarrier *b,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    if(!b || !ps5vk_tiled_2d_sampled_color_image(b->image))return 0;
    const VkAccessFlags next=VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags stages=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags d3d9_stages=stages |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    return (b->oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            b->srcAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
            b->dstAccessMask==next &&
            src_stage==VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
            (dst_stage==stages ||
             (b->image->info.format==VK_FORMAT_B8G8R8A8_UNORM &&
              dst_stage==d3d9_stages))) ||
           (b->oldLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT &&
            src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
            dst_stage==VK_PIPELINE_STAGE_TRANSFER_BIT) ||
           (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
            b->dstAccessMask==next && src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
            (dst_stage==stages ||
             (b->image->info.format==VK_FORMAT_B8G8R8A8_UNORM &&
              dst_stage==d3d9_stages))) ||
           (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
           b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
           !b->srcAccessMask && b->dstAccessMask==next &&
           src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
           (dst_stage==(VkPipelineStageFlags)(stages |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT) ||
            /* D3D9's GetRenderTargetData return names vertex and geometry
             * readers as well as the host publication in this one barrier. */
            (b->image->info.format==VK_FORMAT_B8G8R8A8_UNORM &&
             dst_stage==(VkPipelineStageFlags)(d3d9_stages |
                 VK_PIPELINE_STAGE_HOST_BIT))));
}
#endif

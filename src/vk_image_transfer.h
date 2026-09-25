#ifndef PS5VK_IMAGE_TRANSFER_H
#define PS5VK_IMAGE_TRANSFER_H
#include "vk_command.h"
#include <string.h>

/* Bounded image copy / colour clear domains.
 *
 * RGBA8 images whose usage is drawn from TRANSFER_SRC/TRANSFER_DST alone use
 * the padded linear frontend layout (256-byte row pitch). Tiled array/input
 * colour attachments never enter that executor: whole-array clears and
 * in-render-pass colour rectangles use ordered native GPU DMA packets.
 *
 * vkCmdClearDepthStencilImage executes for the whole subresource of a one-
 * sample D32_SFLOAT target that also carries transfer-destination usage. A
 * constant depth value is the same 32-bit word in every texel, so the uniform
 * DWORD fill the render pass already uses for its depth load-op clear writes
 * exactly the right image without the 64KB_Z_X pixel equations this codebase
 * still does not claim; partial ranges, rectangles, stencil aspects and
 * multisample images therefore remain fail-closed for depth/stencil clears.
 * vkCmdClearAttachments records bounded RGBA8/BGRA8 colour rectangles and the
 * native backend orders prior colour writes, tiled fills and later rendering.
 *
 * Frontend effects execute in start_submission when their segment reaches the
 * head, never at record time; the memory backend flushes each frontend write.
 * GPU effects commit resource state only after exact completion is observed. */
VkBool32 ps5vk_image_transfer_operation(enum ps5vk_operation_type type);

/* Which executor owns a recorded image operation: the pure transfer role's row
 * memcpy (TRANSFER) or the linear frontend's padded-linear work, which now
 * includes the colour-attachment readback copy into the staging image (LINEAR).
 * Exactly one domain owns an operation, so the two executors cannot both act on
 * it. */
enum ps5vk_image_domain {
    PS5VK_IMAGE_DOMAIN_NONE,
    PS5VK_IMAGE_DOMAIN_TRANSFER,
    PS5VK_IMAGE_DOMAIN_LINEAR
};
enum ps5vk_image_domain ps5vk_image_domain(const struct ps5vk_operation *operation);

/* VideoOut's BGRA8 transfer destination uses the tiled colour footprint,
 * whether or not it also declares colour-attachment usage. */
static inline VkBool32 ps5vk_bgra8_transfer_target(VkImage image)
{
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return image && image->info.format == VK_FORMAT_B8G8R8A8_UNORM &&
        image->info.imageType == VK_IMAGE_TYPE_2D && !image->info.flags &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        image->info.extent.depth == 1 && image->info.mipLevels == 1 &&
        image->info.arrayLayers == 1 && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        (image->info.usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT ||
         image->info.usage == usage);
}

/* One colour subresource copied from a linear buffer into the tiled target.
 * The executor uses the measured 64KB_R_X address equation for each texel. */
static inline VkBool32 ps5vk_bgra8_buffer_copy_region(
    VkImage image, VkDeviceSize buffer_bytes, const VkBufferImageCopy *r)
{
    if (!ps5vk_bgra8_transfer_target(image) || !r ||
        r->imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
        r->imageSubresource.mipLevel || r->imageSubresource.baseArrayLayer ||
        r->imageSubresource.layerCount != 1 || r->imageOffset.x < 0 ||
        r->imageOffset.y < 0 || r->imageOffset.z || !r->imageExtent.width ||
        !r->imageExtent.height || r->imageExtent.depth != 1 ||
        r->bufferOffset % 4 ||
        (r->bufferRowLength && r->bufferRowLength < r->imageExtent.width) ||
        (r->bufferImageHeight && r->bufferImageHeight < r->imageExtent.height) ||
        (uint32_t)r->imageOffset.x > image->info.extent.width ||
        (uint32_t)r->imageOffset.y > image->info.extent.height ||
        r->imageExtent.width > image->info.extent.width - (uint32_t)r->imageOffset.x ||
        r->imageExtent.height > image->info.extent.height - (uint32_t)r->imageOffset.y)
        return VK_FALSE;
    const uint64_t pitch = (uint64_t)(r->bufferRowLength ?
        r->bufferRowLength : r->imageExtent.width) * 4u;
    const uint64_t row = (uint64_t)r->imageExtent.width * 4u;
    if (pitch && (r->imageExtent.height - 1u) > (UINT64_MAX - row) / pitch)
        return VK_FALSE;
    const uint64_t span = (uint64_t)(r->imageExtent.height - 1u) * pitch + row;
    return r->bufferOffset <= buffer_bytes &&
        span <= buffer_bytes - r->bufferOffset;
}

static inline VkBool32 ps5vk_bgra8_transfer_barrier(const VkImageMemoryBarrier *b)
{
    if (!b || !ps5vk_bgra8_transfer_target(b->image)) return VK_FALSE;
    const VkAccessFlags color = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags transfer = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkBool32 has_color =
        (b->image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    return (b->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            !b->srcAccessMask && b->dstAccessMask == transfer) ||
        (has_color && b->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         b->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
         b->srcAccessMask == transfer && b->dstAccessMask == color) ||
        (has_color && b->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
         b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         b->srcAccessMask == color && b->dstAccessMask == transfer);
}

/* A software compositor imports an acquired swapchain image for a transfer
 * write and releases it back to the display. The frontend's CPU copy writes
 * the tiled scanout memory directly, so on the queue these are layout
 * bookkeeping plus the ordinary acquire: exactly the two present forms the
 * recorder accepts for a swapchain-owned transfer target (vk_command.c). */
static inline VkBool32 ps5vk_bgra8_present_barrier(const VkImageMemoryBarrier *b)
{
    if (!b || !ps5vk_bgra8_transfer_target(b->image) || !b->image->swapchain_owned)
        return VK_FALSE;
    const VkAccessFlags transfer = VK_ACCESS_TRANSFER_WRITE_BIT;
    return (b->oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
            b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            !b->srcAccessMask && b->dstAccessMask == transfer) ||
        (b->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         b->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
         b->srcAccessMask == transfer &&
         (!b->dstAccessMask || b->dstAccessMask == VK_ACCESS_MEMORY_READ_BIT));
}

static inline VkBool32 ps5vk_d32_gather_barrier(const VkImageMemoryBarrier *b)
{
    if (!b || !ps5vk_d32_gather_image(b->image)) return VK_FALSE;
    return (b->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            !b->srcAccessMask && b->dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT) ||
        (b->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            b->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            b->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT &&
            b->dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
}

/* Array/input-attachment colour surfaces use the native tiled allocation,
 * never the legacy single-layer padded-linear transfer executor. */
static inline VkBool32 ps5vk_array_color_image(VkImage image)
{
    if (!image) return VK_FALSE;
    const VkImageCreateInfo *i = &image->info;
    const VkImageUsageFlags allowed = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return i->format == VK_FORMAT_R8G8B8A8_UNORM &&
        i->imageType == VK_IMAGE_TYPE_2D && i->tiling == VK_IMAGE_TILING_OPTIMAL &&
        i->samples == VK_SAMPLE_COUNT_1_BIT && i->mipLevels == 1 &&
        i->arrayLayers && i->extent.depth == 1 && !i->flags &&
        (i->arrayLayers > 1 || (i->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) &&
        (i->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(i->usage & ~allowed);
}

static inline VkBool32 ps5vk_array_color_barrier(const VkImageMemoryBarrier *b)
{
    if (!b || !ps5vk_array_color_image(b->image)) return VK_FALSE;
    return
        ((b->image->info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
         b->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
         b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         !b->srcAccessMask && b->dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT) ||
        (b->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         b->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
         b->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT &&
         b->dstAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) ||
        (b->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
         b->newLayout == VK_IMAGE_LAYOUT_GENERAL &&
         b->srcAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
         b->dstAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) ||
        ((b->image->info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
         (b->oldLayout == VK_IMAGE_LAYOUT_GENERAL ||
          b->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) &&
         b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
         b->srcAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
         b->dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT);
}

/* The two colour images an explicit clear may fill: the layered one the array
 * path owns, and the plain colour attachment that also declares a transfer
 * destination, which the pinned upstream draw helper clears OUTSIDE the render
 * pass instead of through a LOAD_OP_CLEAR attachment (vkImageUtil.cpp
 * clearColorImage). Both are filled the same way, with one uniform DWORD over
 * the whole allocation, so the tiling equations are neither needed nor claimed. */
static inline VkBool32 ps5vk_explicit_color_clear_image(VkImage image)
{
    return image && (ps5vk_array_color_image(image) ||
                     ps5vk_colour_transfer_image(image) ||
                     ps5vk_bgra8_transfer_target(image));
}
static inline VkBool32 ps5vk_array_color_clear(const struct ps5vk_operation *op)
{
    if (!op || op->type != PS5VK_CLEAR_COLOR_IMAGE ||
        !ps5vk_explicit_color_clear_image(op->image_destination) ||
        !(op->image_destination->info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        (op->image_destination_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         op->image_destination_layout != VK_IMAGE_LAYOUT_GENERAL) ||
        !op->owned_payload || !op->image_region_count ||
        op->owned_payload_size != (size_t)op->image_region_count * sizeof(VkImageSubresourceRange))
        return VK_FALSE;
    const VkImageSubresourceRange *ranges = op->owned_payload;
    for (uint32_t n = 0; n < op->image_region_count; ++n) {
        const VkImageSubresourceRange *r = &ranges[n];
        if (r->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || r->baseMipLevel || r->baseArrayLayer ||
            (r->levelCount != 1 && r->levelCount != VK_REMAINING_MIP_LEVELS) ||
            (r->layerCount != op->image_destination->info.arrayLayers &&
             r->layerCount != VK_REMAINING_ARRAY_LAYERS)) return VK_FALSE;
    }
    return VK_TRUE;
}

static inline VkBool32 ps5vk_d16_attachment_image(VkImage image);

static inline VkBool32 ps5vk_clear_attachment_valid(const struct ps5vk_operation *op)
{
    if(!op || op->type!=PS5VK_CLEAR_ATTACHMENT || !op->render_pass || !op->framebuffer ||
       op->subpass>=op->render_pass->subpass_count ||
       op->render_pass_contents!=VK_SUBPASS_CONTENTS_INLINE)return VK_FALSE;
    const struct ps5vk_subpass *s=ps5vk_render_pass_subpass(op->render_pass,op->subpass);
    if(!s)return VK_FALSE;
    const VkBool32 depth=ps5vk_d16_attachment_image(op->image_destination);
    const uint32_t attachment=depth?s->depth.attachment:s->color[0].attachment;
    if(attachment>=op->framebuffer->attachment_count)return VK_FALSE;
    VkImageView view=op->framebuffer->attachments[attachment];
    if(!view || !view->image || view->image!=op->image_destination ||
       view->range.baseMipLevel || view->range.levelCount!=1 ||
       !view->range.layerCount || view->range.layerCount>32 ||
       view->range.baseArrayLayer>=view->image->info.arrayLayers ||
       view->range.layerCount>view->image->info.arrayLayers-view->range.baseArrayLayer)
        return VK_FALSE;
    const VkImageCreateInfo *image=&view->image->info;
    if(depth) {
        if(view->range.aspectMask!=VK_IMAGE_ASPECT_DEPTH_BIT ||
           !ps5vk_d16_attachment_image(view->image))return VK_FALSE;
    } else if(view->range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
       (image->format!=VK_FORMAT_R8G8B8A8_UNORM && image->format!=VK_FORMAT_B8G8R8A8_UNORM) ||
       !ps5vk_sample_count_implemented(image->samples) || image->mipLevels!=1 ||
       image->imageType!=VK_IMAGE_TYPE_2D || image->tiling!=VK_IMAGE_TILING_OPTIMAL ||
       !(image->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))return VK_FALSE;
    const VkClearRect *r=&op->clear_rect;
    const VkRect2D *area=&op->render_area;
    if(r->baseArrayLayer || r->layerCount!=1 || area->offset.x<0 || area->offset.y<0 ||
       r->rect.offset.x<area->offset.x || r->rect.offset.y<area->offset.y ||
       !r->rect.extent.width || !r->rect.extent.height)return VK_FALSE;
    uint32_t x=(uint32_t)(r->rect.offset.x-area->offset.x);
    uint32_t y=(uint32_t)(r->rect.offset.y-area->offset.y);
    if(x>area->extent.width || r->rect.extent.width>area->extent.width-x ||
       y>area->extent.height || r->rect.extent.height>area->extent.height-y)return VK_FALSE;
    if(depth && (area->offset.x || area->offset.y || r->rect.offset.x || r->rect.offset.y ||
        area->extent.width!=128u || area->extent.height!=128u ||
        r->rect.extent.width!=128u || r->rect.extent.height!=128u))return VK_FALSE;
    /* A multisampled attachment is cleared by one whole-surface fill, which
     * covers every sample of every texel whatever order the hardware stores
     * them in. The rect equation addresses a single sample plane, so it is
     * only honest when the clear IS the whole surface: the rect has to cover
     * the render area and the render area the whole image (DXVK262-T06). */
    if(image->samples!=VK_SAMPLE_COUNT_1_BIT &&
       (x || y || r->rect.extent.width!=area->extent.width ||
        r->rect.extent.height!=area->extent.height ||
        area->offset.x || area->offset.y ||
        area->extent.width!=image->extent.width ||
        area->extent.height!=image->extent.height))return VK_FALSE;
    uint32_t mask=op->render_pass->multiview.present?
        op->render_pass->multiview.view_masks[op->subpass]:0;
    return view->range.layerCount==32 || !(mask>>view->range.layerCount);
}

/* The depth role vkCmdClearDepthStencilImage accepts: a one-sample D32_SFLOAT
 * 2D target carrying the transfer-destination usage the clear consumes. The
 * depth/stencil attachment usage may accompany it but is not required, because
 * Vulkan asks only for TRANSFER_DST on a cleared image. mipLevels and
 * arrayLayers are already forced to one by ps5vk_native_image_requirements for
 * every D32 image. Inline so the recorder, the submit-time gate and the native
 * emitter share one definition instead of three. */
static inline VkBool32 ps5vk_depth_clear_image(VkImage image)
{
    return image && image->info.format == VK_FORMAT_D32_SFLOAT &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 1 &&
        image->info.extent.depth == 1 &&
        (image->info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? VK_TRUE : VK_FALSE;
}

/* The diagnostic synchronization leaf uses one depth-only D16 attachment.
 * It has no sampled or transfer role, so its only image barrier is the
 * transition from undefined contents into attachment writes. */
static inline VkBool32 ps5vk_d16_attachment_image(VkImage image)
{
    return image && image->info.format == VK_FORMAT_D16_UNORM &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.extent.width == 128u && image->info.extent.height == 128u &&
        image->info.extent.depth == 1u && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL && image->info.mipLevels == 1u &&
        image->info.arrayLayers == 1u && !image->info.flags &&
        image->info.usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
}

/* A D32_SFLOAT depth value in [0,1] as the 32-bit word the surface stores.
 * The comparison rejects NaN, and the render pass load-op clear encodes the
 * identical bits, so both clear paths agree byte for byte. */
static inline VkBool32 ps5vk_depth_clear_word(float depth, uint32_t *out)
{
    if (!out || !(depth >= 0.0f && depth <= 1.0f)) return VK_FALSE;
    memcpy(out, &depth, sizeof(*out));
    return VK_TRUE;
}

/* A render-pass depth clear is expanded over the allocation by the native
 * queue. D16 stores one normalized 16-bit value per texel; repeating that
 * halfword in the 32-bit DMA pattern clears both adjacent texels. The D16
 * format remains restricted to its diagnostic 128x128 attachment shape. */
static inline VkBool32 ps5vk_depth_attachment_clear_word(VkFormat format,
    float depth, uint32_t *out)
{
    if (!out || !(depth >= 0.0f && depth <= 1.0f)) return VK_FALSE;
    /* The depth plane of D32_SFLOAT_S8_UINT is the same Z_32_FLOAT word. */
    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        memcpy(out, &depth, sizeof(*out));
        return VK_TRUE;
    }
    if (format != VK_FORMAT_D16_UNORM) return VK_FALSE;
    const uint32_t value = (uint32_t)(depth * 65535.0f + 0.5f);
    const uint32_t half = value & UINT32_C(0xffff);
    *out = half | (half << 16u);
    return VK_TRUE;
}
/* True when this recorded operation is frontend work of the pure transfer
 * role. The render-target readback prelude/postlude keeps its GPU path. */
VkBool32 ps5vk_image_linear_operation(const struct ps5vk_operation *operation);
VkResult ps5vk_image_transfer_execute(VkDevice device, const struct ps5vk_operation *operation);
VkResult ps5vk_image_linear_execute(VkDevice device, const struct ps5vk_operation *operation);
/* Re-validate a recorded image operation against the live device state; used by
 * submit-time command validation and again before execution at the head. */
VkResult ps5vk_image_transfer_validate(VkDevice device, const struct ps5vk_operation *operation);
VkResult ps5vk_image_linear_validate(VkDevice device, const struct ps5vk_operation *operation);
/* Record-time region gate for the padded-linear transfer role: validates one
 * base-level RGBA8 area between the buffer and the image in either direction
 * before any operation is appended. */
VkResult ps5vk_image_linear_region_validate(VkImage image, const VkBufferImageCopy *region,
    VkDeviceSize buffer_bytes, VkDeviceSize image_bytes);

#endif

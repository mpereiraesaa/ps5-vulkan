#ifndef PS5VK_IMAGE_TRANSFER_H
#define PS5VK_IMAGE_TRANSFER_H
#include "vk_command.h"
#include <string.h>

/* Vulkan 1.0 image copy / colour clear domain.
 *
 * Only the role this profile can describe truthfully is implemented: an RGBA8
 * image whose usage is drawn from TRANSFER_SRC/TRANSFER_DST alone, which the
 * driver backs with the padded linear layout used by the upload path (256-byte
 * row pitch). The tiled colour-attachment layout has no linear addressing and
 * is refused here rather than faked with a linear memset.
 *
 * vkCmdClearDepthStencilImage executes for the whole subresource of a one-
 * sample D32_SFLOAT target that also carries transfer-destination usage. A
 * constant depth value is the same 32-bit word in every texel, so the uniform
 * DWORD fill the render pass already uses for its depth load-op clear writes
 * exactly the right image without the 64KB_Z_X pixel equations this codebase
 * still does not claim; partial ranges, rectangles, stencil aspects and
 * multisample images therefore remain fail-closed. vkCmdClearAttachments is
 * exposed and fully validated but fail closed: a mid-render-pass attachment
 * clear would need a DCB clear path that does not exist yet.
 *
 * Effects execute in start_submission when the frontend segment reaches the
 * head, never at record time; the destination allocation range is flushed
 * through the memory backend after each driver-originated write. */
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

static inline VkBool32 ps5vk_array_color_clear(const struct ps5vk_operation *op)
{
    if (!op || op->type != PS5VK_CLEAR_COLOR_IMAGE ||
        !ps5vk_array_color_image(op->image_destination) ||
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

static inline VkBool32 ps5vk_clear_attachment_valid(const struct ps5vk_operation *op)
{
    if(!op || op->type!=PS5VK_CLEAR_ATTACHMENT || !op->render_pass || !op->framebuffer ||
       op->subpass>=op->render_pass->subpass_count ||
       op->render_pass_contents!=VK_SUBPASS_CONTENTS_INLINE)return VK_FALSE;
    const struct ps5vk_subpass *s=ps5vk_render_pass_subpass(op->render_pass,op->subpass);
    if(!s || s->color.attachment>=op->framebuffer->attachment_count)return VK_FALSE;
    VkImageView view=op->framebuffer->attachments[s->color.attachment];
    if(!view || !view->image || view->image!=op->image_destination ||
       view->range.baseMipLevel || view->range.levelCount!=1 ||
       !view->range.layerCount || view->range.layerCount>32 ||
       view->range.baseArrayLayer>=view->image->info.arrayLayers ||
       view->range.layerCount>view->image->info.arrayLayers-view->range.baseArrayLayer)
        return VK_FALSE;
    const VkImageCreateInfo *image=&view->image->info;
    if((image->format!=VK_FORMAT_R8G8B8A8_UNORM && image->format!=VK_FORMAT_B8G8R8A8_UNORM) ||
       image->samples!=VK_SAMPLE_COUNT_1_BIT || image->mipLevels!=1 ||
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

/* A D32_SFLOAT depth value in [0,1] as the 32-bit word the surface stores.
 * The comparison rejects NaN, and the render pass load-op clear encodes the
 * identical bits, so both clear paths agree byte for byte. */
static inline VkBool32 ps5vk_depth_clear_word(float depth, uint32_t *out)
{
    if (!out || !(depth >= 0.0f && depth <= 1.0f)) return VK_FALSE;
    memcpy(out, &depth, sizeof(*out));
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

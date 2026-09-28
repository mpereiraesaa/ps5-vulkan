#ifndef PS5VK_DXVK_CLEAR_PASS_H
#define PS5VK_DXVK_CLEAR_PASS_H

#include "vk_command.h"
#include "vk_framebuffer.h"

/* DXVK 2.6.2 clears a full BGRA8 backbuffer with a dynamic-rendering pass
 * whose only observable work is its loadOp=CLEAR. The queue and native
 * executor use the same narrow shape before admitting a zero-body pass. */
static inline int ps5vk_dxvk_bgra8_clear_only_pass(const struct ps5vk_operation *begin)
{
    if (!begin || begin->type != PS5VK_BEGIN_RENDER_PASS ||
        !begin->render_pass || !begin->framebuffer ||
        begin->render_pass_contents != VK_SUBPASS_CONTENTS_INLINE ||
        begin->subpass || begin->clear_count != 1)
        return 0;
    VkRenderPass pass = begin->render_pass;
    VkFramebuffer fb = begin->framebuffer;
    if (pass->attachment_count != 1 || pass->subpass_count != 1 ||
        pass->dependency_count || pass->input_count || pass->preserve_count ||
        pass->multiview.present || !pass->attachments || !pass->subpasses ||
        fb->attachment_count != 1 || fb->color_count != 1 ||
        fb->color_attachments[0] != 0 || !fb->attachments[0] ||
        begin->render_area.offset.x || begin->render_area.offset.y ||
        begin->render_area.extent.width != fb->width ||
        begin->render_area.extent.height != fb->height)
        return 0;
    const struct ps5vk_subpass *subpass = &pass->subpasses[0];
    const VkAttachmentDescription *attachment = &pass->attachments[0];
    VkImageView view = fb->attachments[0];
    VkImage image = view->image;
    return subpass->color_count == 1 &&
        subpass->color[0].attachment == 0 &&
        subpass->color[0].layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        subpass->depth.attachment == VK_ATTACHMENT_UNUSED &&
        !subpass->resolve_count && !subpass->input_count &&
        attachment->format == VK_FORMAT_B8G8R8A8_UNORM &&
        attachment->samples == VK_SAMPLE_COUNT_1_BIT &&
        attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
        attachment->storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
        attachment->initialLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        attachment->finalLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        view->format == VK_FORMAT_B8G8R8A8_UNORM &&
        view->range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
        !view->range.baseMipLevel && view->range.levelCount == 1 &&
        !view->range.baseArrayLayer && view->range.layerCount == 1 &&
        ps5vk_tiled_2d_sampled_color_image(image) &&
        image->info.format == VK_FORMAT_B8G8R8A8_UNORM &&
        image->info.extent.width == fb->width &&
        image->info.extent.height == fb->height;
}

#endif

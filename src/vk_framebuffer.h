#ifndef PS5VK_FRAMEBUFFER_H
#define PS5VK_FRAMEBUFFER_H
#include "vk_render_pass.h"
#include "vk_image.h"
struct VkFramebuffer_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    uint32_t width, height, attachment_count;
    VkImageView attachments[2];
    VkFormat formats[2];
    VkSampleCountFlagBits samples[2];
    uint32_t color_attachment, depth_attachment;
};
/* One ROLE of a framebuffer against the render-pass reference that names it.
 * Both unused is compatible; otherwise both must be used and the attachment
 * each one refers to must agree on format and sample count. The numeric slots
 * are deliberately not compared: the same role may sit at a different index on
 * each side, and attachments no reference names are irrelevant. */
static inline VkBool32 role_compatible(VkFramebuffer fb, uint32_t slot,
    VkRenderPass pass, const VkAttachmentReference *reference)
{
    const VkBool32 fb_used = slot != VK_ATTACHMENT_UNUSED ? VK_TRUE : VK_FALSE;
    const VkBool32 pass_used =
        reference->attachment != VK_ATTACHMENT_UNUSED ? VK_TRUE : VK_FALSE;
    if (fb_used != pass_used) return VK_FALSE;
    if (!fb_used) return VK_TRUE;
    if (slot >= fb->attachment_count ||
        reference->attachment >= pass->attachment_count) return VK_FALSE;
    const VkAttachmentDescription *described = &pass->attachments[reference->attachment];
    return fb->formats[slot] == described->format &&
        fb->samples[slot] == described->samples ? VK_TRUE : VK_FALSE;
}

/* Vulkan 1.0 chapter 7.2 framebuffer compatibility, by role rather than by
 * slot. This is the ONLY definition in the driver: a second, stricter copy
 * one function away is how a conformant continuation secondary ended up
 * refused for naming a compatible framebuffer.
 *
 * Load/store ops and layouts are intentionally not compatibility keys, and
 * neither are the attachment count or the numeric indices. The stricter,
 * index-identical requirement that vkCmdBeginRenderPass places on the
 * framebuffer that actually EXECUTES is a separate boundary of the native
 * path, which indexes attachments positionally; it is not this rule. */
static inline VkBool32 ps5vk_framebuffer_compatible(VkFramebuffer fb, VkRenderPass pass)
{
    if (!fb || !pass || fb->device != pass->device) return VK_FALSE;
    /* Every subpass of this profile draws into the same colour and depth
     * roles, so the framebuffer has to satisfy each of them; a pass whose
     * subpasses disagreed on a role would be refused by a framebuffer that
     * can only carry one of each. */
    for (uint32_t i = 0; i < pass->subpass_count; ++i) {
        const struct ps5vk_subpass *subpass = ps5vk_render_pass_subpass(pass, i);
        if (!role_compatible(fb, fb->color_attachment, pass, &subpass->color[0]) ||
            !role_compatible(fb, fb->depth_attachment, pass, &subpass->depth))
            return VK_FALSE;
    }
    return VK_TRUE;
}
#endif

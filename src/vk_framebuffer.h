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
    /* One slot per attachment the pass may name, which is the same bound the
     * render pass enforces: the pinned multisample oracle's framebuffer carries
     * the multisampled colour attachment, its resolve target and one
     * single-sample target per fetched sample, and a framebuffer sized for two
     * slots wrote past itself the moment the pass bound followed that shape.
     * The colour and resolve ROLE lists stay bounded by the colour-attachment
     * contract, because that is how many targets a draw may write. */
    VkImageView attachments[PS5VK_MAX_ATTACHMENTS];
    VkFormat formats[PS5VK_MAX_ATTACHMENTS];
    VkSampleCountFlagBits samples[PS5VK_MAX_ATTACHMENTS];
    /* The colour roles this framebuffer carries, in the order the subpass names
     * them, and the depth role. The count is the pass's own, bounded by the
     * colour-attachment contract; every consumer reads index 0 while the bound is
     * one. */
    uint32_t color_attachments[PS5VK_MAX_COLOR_ATTACHMENTS], color_count;
    /* The resolve role of each colour role, in the same order, or
     * VK_ATTACHMENT_UNUSED when the pass declares a resolve array with an
     * unused entry. resolve_count is the number of entries the pass DECLARED,
     * so a framebuffer built by hand with no resolve role keeps the zero value
     * and cannot accidentally name attachment 0 (DXVK262-T06). */
    uint32_t resolve_count;
    uint32_t resolve_attachments[PS5VK_MAX_COLOR_ATTACHMENTS];
    uint32_t depth_attachment;
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
        if (fb->color_count != subpass->color_count) return VK_FALSE;
        if (fb->resolve_count != subpass->resolve_count) return VK_FALSE;
        for (uint32_t c = 0; c < subpass->color_count; ++c)
            if (!role_compatible(fb, fb->color_attachments[c], pass, &subpass->color[c]) ||
                (c < subpass->resolve_count &&
                 !role_compatible(fb, fb->resolve_attachments[c], pass, &subpass->resolve[c])))
                return VK_FALSE;
        if (!role_compatible(fb, fb->depth_attachment, pass, &subpass->depth))
            return VK_FALSE;
    }
    return VK_TRUE;
}
#endif

#include "vk_framebuffer.h"
#include <string.h>

/* The array layers an attachment view has to carry for a pass that uses view
 * masks: one layer per view, so the highest view index any subpass that names
 * this attachment renders, plus one. Zero means the pass has no masks at all,
 * which is every pass the shipping build can create, and zero adds no
 * requirement anywhere below. */
static uint32_t attachment_view_count(VkRenderPass pass, uint32_t attachment)
{
    const struct ps5vk_render_pass_multiview *multiview = &pass->multiview;
    if (!multiview->present) return 0;
    uint32_t views = 0;
    for (uint32_t s = 0; s < multiview->subpass_count; ++s) {
        const struct ps5vk_subpass *subpass = ps5vk_render_pass_subpass(pass, s);
        if (subpass->color.attachment != attachment &&
            subpass->depth.attachment != attachment) continue;
        const uint32_t mask = multiview->view_masks[s];
        /* A view mask is 32 bits wide, so a view index is a bit position. */
        for (uint32_t bit = 0; bit < 32u; ++bit)
            if (mask & (UINT32_C(1) << bit)) views = bit + 1u;
    }
    return views;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkFramebuffer *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO) return VK_ERROR_UNKNOWN;
    if (!d->graphics_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pNext || info->flags || info->layers != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkRenderPass pass = info->renderPass;
    if (!pass || pass->device != d || !info->width || !info->height ||
        info->attachmentCount != pass->attachment_count || !info->pAttachments)
        return VK_ERROR_UNKNOWN;
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        VkImageView view = info->pAttachments[i];
        if (!view || view->device != d || !view->image->memory || view->range.levelCount != 1 ||
            view->format != pass->attachments[i].format ||
            view->image->info.samples != pass->attachments[i].samples) return VK_ERROR_UNKNOWN;
        VkImageUsageFlags usage = i == ps5vk_render_pass_subpass(pass, 0)->depth.attachment ?
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        uint32_t mip = view->range.baseMipLevel;
        if (mip >= 32) return VK_ERROR_UNKNOWN;
        uint32_t width = view->image->info.extent.width >> mip;
        uint32_t height = view->image->info.extent.height >> mip;
        if (!width) width = 1;
        if (!height) height = 1;
        if (!(view->image->info.usage & usage) || info->width > width || info->height > height)
            return VK_ERROR_UNKNOWN;
        /* A pass with view masks renders each subpass once per view, into the
         * attachment view's own layers, so that view has to START at layer zero
         * and carry every view the subpasses naming it render. The view's range
         * was already validated against the image it was created on
         * (vkCreateImageView), so a backing with fewer layers than the mask
         * needs cannot satisfy this and is refused here. The framebuffer itself
         * still has exactly one layer. */
        const uint32_t views = attachment_view_count(pass, i);
        if (views && (view->range.baseArrayLayer || view->range.layerCount < views))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkFramebuffer fb = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, allocator,
        sizeof(*fb), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!fb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(fb, 0, sizeof(*fb)); fb->device = d; fb->allocator = saved; fb->custom_allocator = custom;
    fb->width = info->width; fb->height = info->height; fb->attachment_count = info->attachmentCount;
    /* The roles are the same in every subpass of this profile, so the first
     * one names them for the framebuffer. */
    fb->color_attachment = ps5vk_render_pass_subpass(pass, 0)->color.attachment;
    fb->depth_attachment = ps5vk_render_pass_subpass(pass, 0)->depth.attachment;
    for (uint32_t i = 0; i < fb->attachment_count; ++i) {
        fb->attachments[i] = info->pAttachments[i]; ++fb->attachments[i]->framebuffers;
        fb->formats[i] = pass->attachments[i].format; fb->samples[i] = pass->attachments[i].samples;
    }
    ++d->graphics_objects; *out = fb; return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice d, VkFramebuffer fb,
                                                const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!fb) return;
    if (!d || fb->device != d || fb->pending ||
        (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_FRAMEBUFFER, fb))) {
        if (d) ++d->lifetime_errors;
        return;
    }
    for (uint32_t i = 0; i < fb->attachment_count; ++i) --fb->attachments[i]->framebuffers;
    --d->graphics_objects;
    VkAllocationCallbacks saved = fb->allocator; VkBool32 custom = fb->custom_allocator;
    ps5vk_object_free(fb, &saved, custom);
}

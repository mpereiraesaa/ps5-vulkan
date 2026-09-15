#include "vk_framebuffer.h"
#include <string.h>

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

#include "vk_framebuffer.h"
#include "color_attachment_contract.h"
#include <string.h>

#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#define FB_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define FB_MARK(...) ((void)0)
#endif

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkFramebuffer *out)
{
    FB_MARK("PS5VK_FRAMEBUFFER_CREATE attachments=%u extent=%ux%u layers=%u",
        info ? info->attachmentCount : 0u, info ? info->width : 0u,
        info ? info->height : 0u, info ? info->layers : 0u);
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO) return VK_ERROR_UNKNOWN;
    if (!d->graphics_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkBool32 imageless = !!(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT);
    if (info->flags & ~VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT || info->layers != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (imageless && !(d->enabled_features_t09 & PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkFramebufferAttachmentsCreateInfo *attachments_info = NULL;
    if (imageless) {
        attachments_info = (const VkFramebufferAttachmentsCreateInfo *)info->pNext;
        if (!attachments_info ||
            attachments_info->sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO ||
            attachments_info->pNext ||
            attachments_info->attachmentImageInfoCount != info->attachmentCount ||
            (info->attachmentCount && !attachments_info->pAttachmentImageInfos))
            return VK_ERROR_UNKNOWN;
    } else if (info->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkRenderPass pass = info->renderPass;
    if (!pass || pass->device != d || !info->width || !info->height ||
        info->attachmentCount != pass->attachment_count ||
        (!imageless && info->attachmentCount && !info->pAttachments))
        return VK_ERROR_UNKNOWN;
    if (!imageless) for (uint32_t i = 0; i < info->attachmentCount; ++i) {
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
        const uint32_t views = ps5vk_framebuffer_attachment_view_count(pass, i);
        if (views && (view->range.baseArrayLayer || view->range.layerCount < views))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkFramebuffer fb = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, allocator,
        sizeof(*fb), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!fb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(fb, 0, sizeof(*fb)); fb->device = d; fb->allocator = saved; fb->custom_allocator = custom;
    fb->imageless = imageless;
    fb->width = info->width; fb->height = info->height; fb->attachment_count = info->attachmentCount;
    /* The roles are the same in every subpass of this profile, so the first
     * one names them for the framebuffer. */
    {
        const struct ps5vk_subpass *first = ps5vk_render_pass_subpass(pass, 0);
        fb->color_count = first->color_count;
        fb->resolve_count = first->resolve_count;
        for (uint32_t c = 0; c < first->color_count; ++c) {
            fb->color_attachments[c] = first->color[c].attachment;
            /* The resolve role travels with the colour role it resolves. */
            fb->resolve_attachments[c] = first->resolve[c].attachment;
        }
    }
    fb->depth_attachment = ps5vk_render_pass_subpass(pass, 0)->depth.attachment;
    for (uint32_t i = 0; i < fb->attachment_count; ++i) {
        fb->formats[i] = pass->attachments[i].format; fb->samples[i] = pass->attachments[i].samples;
        if (!imageless) {
            fb->attachments[i] = info->pAttachments[i];
            ++fb->attachments[i]->framebuffers;
            continue;
        }
        const VkFramebufferAttachmentImageInfo *a = &attachments_info->pAttachmentImageInfos[i];
        const VkImageUsageFlags usage = i == fb->depth_attachment ?
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (a->sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO || a->pNext ||
            !a->width || !a->height || !a->layerCount ||
            a->width < fb->width || a->height < fb->height ||
            a->layerCount < info->layers || !(a->usage & usage) ||
            !a->viewFormatCount || !a->pViewFormats ||
            a->layerCount < ps5vk_framebuffer_attachment_view_count(pass, i)) goto fail;
        VkBool32 match = VK_FALSE;
        for (uint32_t j = 0; j < a->viewFormatCount; ++j)
            if (a->pViewFormats[j] == fb->formats[i]) match = VK_TRUE;
        if (!match) goto fail;
        fb->view_formats[i] = ps5vk_object_alloc(fb->custom_allocator ? &fb->allocator : NULL,
            NULL, (size_t)a->viewFormatCount * sizeof(VkFormat),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &fb->format_allocators[i],
            &fb->format_custom[i]);
        if (!fb->view_formats[i]) goto oom;
        memcpy(fb->view_formats[i], a->pViewFormats,
            (size_t)a->viewFormatCount * sizeof(VkFormat));
        fb->view_format_count[i] = a->viewFormatCount;
        fb->image_flags[i] = a->flags;
        fb->image_usage[i] = a->usage;
        fb->image_width[i] = a->width;
        fb->image_height[i] = a->height;
        fb->image_layers[i] = a->layerCount;
    }
    ++d->graphics_objects; *out = fb; return VK_SUCCESS;
oom:
    for (uint32_t i = 0; i < fb->attachment_count; ++i)
        if (fb->view_formats[i]) ps5vk_object_free(fb->view_formats[i],
            &fb->format_allocators[i], fb->format_custom[i]);
    ps5vk_object_free(fb, &saved, custom);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
fail:
    for (uint32_t i = 0; i < fb->attachment_count; ++i)
        if (fb->view_formats[i]) ps5vk_object_free(fb->view_formats[i],
            &fb->format_allocators[i], fb->format_custom[i]);
    ps5vk_object_free(fb, &saved, custom);
    return VK_ERROR_UNKNOWN;
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
    for (uint32_t i = 0; i < fb->attachment_count; ++i) {
        if (fb->attachments[i]) --fb->attachments[i]->framebuffers;
        if (fb->view_formats[i]) ps5vk_object_free(fb->view_formats[i],
            &fb->format_allocators[i], fb->format_custom[i]);
    }
    --d->graphics_objects;
    VkAllocationCallbacks saved = fb->allocator; VkBool32 custom = fb->custom_allocator;
    ps5vk_object_free(fb, &saved, custom);
}

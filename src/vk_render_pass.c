#include "vk_render_pass.h"
#include <string.h>

static int layout(VkImageLayout value, int depth, int initial)
{
    return (initial && value == VK_IMAGE_LAYOUT_UNDEFINED) ||
        value == VK_IMAGE_LAYOUT_GENERAL || value == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        value == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        value == (depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
        (!depth && value == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice d,
    const VkRenderPassCreateInfo *info, const VkAllocationCallbacks *allocator, VkRenderPass *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO)
        return VK_ERROR_UNKNOWN;
    if (!d->graphics_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pNext || info->flags || info->subpassCount != 1 || !info->pSubpasses ||
        !info->attachmentCount || info->attachmentCount > 2 || !info->pAttachments ||
        info->dependencyCount > 2 || (info->dependencyCount && !info->pDependencies))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkSubpassDescription *s = info->pSubpasses;
    if (s->flags || s->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        s->colorAttachmentCount != 1 || !s->pColorAttachments || s->inputAttachmentCount ||
        s->pResolveAttachments || s->preserveAttachmentCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkAttachmentReference color = s->pColorAttachments[0];
    VkAttachmentReference depth = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    if (s->pDepthStencilAttachment) depth = *s->pDepthStencilAttachment;
    if (color.attachment >= info->attachmentCount ||
        (color.layout != VK_IMAGE_LAYOUT_GENERAL && color.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
        (depth.attachment != VK_ATTACHMENT_UNUSED && (depth.attachment >= info->attachmentCount ||
            depth.attachment == color.attachment ||
            (depth.layout != VK_IMAGE_LAYOUT_GENERAL && depth.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL))))
        return VK_ERROR_UNKNOWN;
    if (info->attachmentCount != (depth.attachment == VK_ATTACHMENT_UNUSED ? 1u : 2u))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const VkAttachmentDescription *a = &info->pAttachments[i];
        int is_depth = i == depth.attachment;
        if (a->flags || a->samples != VK_SAMPLE_COUNT_1_BIT ||
            (is_depth ? a->format != VK_FORMAT_D32_SFLOAT :
             (a->format != VK_FORMAT_B8G8R8A8_UNORM && a->format != VK_FORMAT_R8G8B8A8_UNORM)))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (a->loadOp < VK_ATTACHMENT_LOAD_OP_LOAD || a->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
            a->storeOp < VK_ATTACHMENT_STORE_OP_STORE || a->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
            a->stencilLoadOp < VK_ATTACHMENT_LOAD_OP_LOAD || a->stencilLoadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
            a->stencilStoreOp < VK_ATTACHMENT_STORE_OP_STORE || a->stencilStoreOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
            !layout(a->initialLayout, is_depth, 1) || !layout(a->finalLayout, is_depth, 0) ||
            (a->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED && a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD))
            return VK_ERROR_UNKNOWN;
    }
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const VkSubpassDependency *dep = &info->pDependencies[i];
        /* Only external entry/exit edges; no self-dependency execution path yet. */
        if (!((dep->srcSubpass == VK_SUBPASS_EXTERNAL && dep->dstSubpass == 0) ||
              (dep->srcSubpass == 0 && dep->dstSubpass == VK_SUBPASS_EXTERNAL)) ||
            dep->dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT ||
            !dep->srcStageMask || !dep->dstStageMask) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkAllocationCallbacks saved; VkBool32 custom;
    VkRenderPass pass = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, allocator,
        sizeof(*pass), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!pass) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(pass, 0, sizeof(*pass)); pass->device = d; pass->allocator = saved; pass->custom_allocator = custom;
    pass->attachment_count = info->attachmentCount; pass->dependency_count = info->dependencyCount;
    memcpy(pass->attachments, info->pAttachments, info->attachmentCount * sizeof(*info->pAttachments));
    if (info->dependencyCount)
        memcpy(pass->dependencies, info->pDependencies, info->dependencyCount * sizeof(*info->pDependencies));
    pass->color = color; pass->depth = depth;
    ++d->graphics_objects; *out = pass; return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice d, VkRenderPass pass,
                                               const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!pass) return;
    if (!d || pass->device != d || pass->pending ||
        (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_RENDER_PASS, pass))) {
        if (d) ++d->lifetime_errors;
        return;
    }
    --d->graphics_objects;
    ps5vk_object_free(pass, &pass->allocator, pass->custom_allocator);
}

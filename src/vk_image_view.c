#include "vk_image.h"
#include <string.h>

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO)
        return VK_ERROR_UNKNOWN;
    if (!d->graphics_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImage image = d->images;
    while (image && image != info->image) image = image->next;
    if (!image || image->device != d || !image->memory) return VK_ERROR_UNKNOWN;
    if (info->pNext || info->flags || info->viewType != VK_IMAGE_VIEW_TYPE_2D ||
        info->format != image->info.format) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkComponentMapping *c = &info->components;
    if ((c->r != VK_COMPONENT_SWIZZLE_IDENTITY && c->r != VK_COMPONENT_SWIZZLE_R) ||
        (c->g != VK_COMPONENT_SWIZZLE_IDENTITY && c->g != VK_COMPONENT_SWIZZLE_G) ||
        (c->b != VK_COMPONENT_SWIZZLE_IDENTITY && c->b != VK_COMPONENT_SWIZZLE_B) ||
        (c->a != VK_COMPONENT_SWIZZLE_IDENTITY && c->a != VK_COMPONENT_SWIZZLE_A))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImageSubresourceRange range = info->subresourceRange;
    VkImageAspectFlags aspect = image->info.format == VK_FORMAT_D32_SFLOAT ?
        VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (range.aspectMask != aspect || range.baseMipLevel >= image->info.mipLevels ||
        range.baseArrayLayer != 0) return VK_ERROR_UNKNOWN;
    if (range.levelCount == VK_REMAINING_MIP_LEVELS)
        range.levelCount = image->info.mipLevels - range.baseMipLevel;
    if (range.layerCount == VK_REMAINING_ARRAY_LAYERS) range.layerCount = 1;
    if (!range.levelCount || range.levelCount > image->info.mipLevels - range.baseMipLevel ||
        range.layerCount != 1) return VK_ERROR_UNKNOWN;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkImageView view = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL,
        allocator, sizeof(*view), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!view) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(view, 0, sizeof(*view));
    view->device = d; view->allocator = saved; view->custom_allocator = custom;
    view->image = image; view->format = info->format; view->range = range;
    ++image->views; ++d->graphics_objects; *out = view;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d, VkImageView view,
                                              const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!view) return;
    if (!d || view->device != d || view->pending || view->framebuffers ||
        (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_IMAGE_VIEW, view))) {
        if (d) ++d->lifetime_errors;
        return;
    }
    --view->image->views; --d->graphics_objects;
    VkAllocationCallbacks saved = view->allocator; VkBool32 custom = view->custom_allocator;
    ps5vk_object_free(view, &saved, custom);
}

#include "vk_image.h"
#include <string.h>

#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#define VIEW_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define VIEW_MARK(...) ((void)0)
#endif

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *out)
{
    /* The image is NOT dereferenced here: this marker runs before the call's
     * own validation, so it may only read the create-info the caller supplied. */
    VIEW_MARK("PS5VK_IMAGE_VIEW format=%u type=%u",
        info ? (unsigned)info->format : 0u,
        info ? (unsigned)info->viewType : 0u);
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO)
        return VK_ERROR_UNKNOWN;
    if (!d->graphics_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImage image = d->images;
    while (image && image != info->image) image = image->next;
    if (!image || image->device != d || !image->memory) return VK_ERROR_UNKNOWN;
    /* Two structures are understood, each at most once; anything else stays
     * fail-closed.
     *
     * The CTS texture helper chains VkImageViewMinLodCreateInfoEXT even when
     * its default minLod is zero. That value is a no-op; nonzero min LOD still
     * needs the unsupported image-view-min-lod feature.
     *
     * VkImageViewUsageCreateInfo (VK_KHR_maintenance2) narrows what the view
     * may be used for. DXVK chains it on every view it creates. It is accepted
     * only on a device that enabled the extension, must name at least one
     * valid usage (VUID-VkImageViewUsageCreateInfo-usage-requiredbitmask and
     * -parameter) and, since this profile never creates an image with
     * VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, only usage the image itself was
     * created with (VUID-VkImageViewCreateInfo-pNext-02662). The narrowed usage
     * is what the framebuffer and descriptor checks then hold the view to. */
    VkImageUsageFlags view_usage = 0;
    VkBool32 saw_min_lod = VK_FALSE;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT &&
            !saw_min_lod) {
            saw_min_lod = VK_TRUE;
            if (((const VkImageViewMinLodCreateInfoEXT *)next)->minLod != 0.0f)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO &&
                   !view_usage && d->maintenance2_extension_enabled) {
            view_usage = ((const VkImageViewUsageCreateInfo *)next)->usage;
            if (!view_usage || (view_usage & ~PS5VK_IMAGE_USAGE_CORE_BITS) ||
                (view_usage & ~image->info.usage))
                return VK_ERROR_UNKNOWN;
        } else {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (info->flags || info->format != image->info.format)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkComponentMapping *c = &info->components;
    if ((c->r != VK_COMPONENT_SWIZZLE_IDENTITY && c->r != VK_COMPONENT_SWIZZLE_R) ||
        (c->g != VK_COMPONENT_SWIZZLE_IDENTITY && c->g != VK_COMPONENT_SWIZZLE_G) ||
        (c->b != VK_COMPONENT_SWIZZLE_IDENTITY && c->b != VK_COMPONENT_SWIZZLE_B) ||
        (c->a != VK_COMPONENT_SWIZZLE_IDENTITY && c->a != VK_COMPONENT_SWIZZLE_A))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImageSubresourceRange range = info->subresourceRange;
    VkImageAspectFlags aspect = (image->info.format == VK_FORMAT_D32_SFLOAT ||
        image->info.format == VK_FORMAT_D16_UNORM) ?
        VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (range.aspectMask != aspect || range.baseMipLevel >= image->info.mipLevels)
        return VK_ERROR_UNKNOWN;
    if (range.levelCount == VK_REMAINING_MIP_LEVELS)
        range.levelCount = image->info.mipLevels - range.baseMipLevel;
    const uint32_t image_layers=image->info.imageType==VK_IMAGE_TYPE_3D?
        1:image->info.arrayLayers;
    if (range.layerCount == VK_REMAINING_ARRAY_LAYERS) {
        if(range.baseArrayLayer>=image_layers)return VK_ERROR_UNKNOWN;
        range.layerCount=image_layers-range.baseArrayLayer;
    }
    if (!range.levelCount || range.levelCount > image->info.mipLevels - range.baseMipLevel ||
        !range.layerCount || range.baseArrayLayer>=image_layers ||
        range.layerCount>image_layers-range.baseArrayLayer) return VK_ERROR_UNKNOWN;
    switch(info->viewType) {
    case VK_IMAGE_VIEW_TYPE_1D:
        if(image->info.imageType!=VK_IMAGE_TYPE_1D || range.layerCount!=1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        break;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        if(image->info.imageType!=VK_IMAGE_TYPE_1D)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        break;
    case VK_IMAGE_VIEW_TYPE_2D:
        if(image->info.imageType!=VK_IMAGE_TYPE_2D || range.layerCount!=1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        break;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        if(image->info.imageType!=VK_IMAGE_TYPE_2D)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
        if(image->info.imageType!=VK_IMAGE_TYPE_2D ||
           !(image->info.flags&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
           range.layerCount!=6 ||
           image->info.extent.width!=image->info.extent.height)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        if(!(d->enabled_features&PS5VK_FEATURE_IMAGE_CUBE_ARRAY) ||
           image->info.imageType!=VK_IMAGE_TYPE_2D ||
           !(image->info.flags&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
           range.layerCount%6 ||
           image->info.extent.width!=image->info.extent.height)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        break;
    case VK_IMAGE_VIEW_TYPE_3D:
        if(image->info.imageType!=VK_IMAGE_TYPE_3D ||
           range.baseArrayLayer || range.layerCount!=1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        break;
    default:return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkImageView view = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL,
        allocator, sizeof(*view), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!view) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(view, 0, sizeof(*view));
    view->device = d; view->allocator = saved; view->custom_allocator = custom;
    view->image = image; view->view_type=info->viewType;
    view->format = info->format; view->range = range;
    view->usage = view_usage;
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

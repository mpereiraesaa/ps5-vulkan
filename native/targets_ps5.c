#include "targets_ps5.h"
#include "depth_layout.h"
#include <string.h>
VkResult ps5vk_native_target(VkDevice d, VkImageView view,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_target_registers *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (!d || !view || view->device != d || view->range.baseMipLevel ||
        view->range.levelCount != 1 || view->range.baseArrayLayer || view->range.layerCount != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImage image = view->image;
    void *address; VkDeviceSize bytes;
    VkResult rc = ps5vk_image_span(d, image, &address, &bytes);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements required;
    rc = ps5vk_native_image_requirements(d, &image->info, &required);
    if (rc != VK_SUCCESS) return rc;
    uintptr_t base = (uintptr_t)address;
    if (bytes < required.size || base % required.alignment || !base ||
        base >= (UINT64_C(1) << 48) || bytes > (UINT64_C(1) << 48) - base) return VK_ERROR_UNKNOWN;
    struct ps5vk_target_registers result = {0};
    uint32_t width = image->info.extent.width, height = image->info.extent.height;
    if (view->format == VK_FORMAT_D32_SFLOAT) {
        if (!(image->info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return VK_ERROR_UNKNOWN;
        if (ps5_depth_build_d32_no_htile(result.registers, base, width, height)) return VK_ERROR_UNKNOWN;
        result.count = PS5_DEPTH_REGISTER_COUNT;
    } else if (view->format == VK_FORMAT_B8G8R8A8_UNORM ||
               view->format == VK_FORMAT_R8G8B8A8_UNORM) {
        if (!(image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return VK_ERROR_UNKNOWN;
        if (ps5_color_build_target(result.registers, color_defaults, base, width, height)) return VK_ERROR_UNKNOWN;
        /* Gears' generic builder selects COMP_SWAP=STD (RGBA byte order).
         * Vulkan BGRA requires ZYXW / SWAP_ALT=1 in CB_COLOR0_INFO[12:11].
         * Public Mesa ac_translate_colorswap + gfx10.json; do not change the
         * read-only shared builder or compensate in application shaders. */
        if (view->format == VK_FORMAT_B8G8R8A8_UNORM)
            result.registers[2].value=(result.registers[2].value & ~UINT32_C(0x1800)) | UINT32_C(0x0800);
        result.count = PS5_COLOR_REGISTER_COUNT;
    } else {
        /* Only the explicitly configured BGRA attachment profile is enabled. */
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    *out = result; return VK_SUCCESS;
}

/* One array layer's footprint, from the same arithmetic the image path uses:
 * the D32 layout for the depth target, the backend's own requirements for the
 * color target (128 KiB-aligned). Both are per-layer quantities, so a
 * layer-addressed surface is layer * footprint bytes. */
VkResult ps5vk_native_layer_footprint(VkDevice d, VkImage image, VkDeviceSize *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = 0;
    if (!d || !image) return VK_ERROR_UNKNOWN;
    if (image->info.format == VK_FORMAT_D32_SFLOAT) {
        struct ps5vk_depth_layout layout;
        if (ps5vk_depth_layout(image->info.extent.width, image->info.extent.height, &layout))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        *out = layout.bytes;
        return VK_SUCCESS;
    }
    VkImageCreateInfo single = image->info;
    single.arrayLayers = 1;
    VkMemoryRequirements requirements;
    VkResult rc = ps5vk_native_image_requirements(d, &single, &requirements);
    if (rc != VK_SUCCESS) return rc;
    *out = requirements.size;
    return VK_SUCCESS;
}

VkResult ps5vk_native_layer_target(VkDevice d, VkImageView view, uint32_t layer,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT],
    struct ps5vk_target_registers *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (!d || !view || view->device != d || view->range.baseMipLevel ||
        view->range.levelCount != 1 || view->range.layerCount != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImage image = view->image;
    const uint64_t target_layer = (uint64_t)view->range.baseArrayLayer + layer;
    if (!image || target_layer >= image->info.arrayLayers)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    void *address; VkDeviceSize bytes;
    VkResult rc = ps5vk_image_span(d, image, &address, &bytes);
    if (rc != VK_SUCCESS) return rc;
    VkDeviceSize footprint = 0;
    rc = ps5vk_native_layer_footprint(d, image, &footprint);
    if (rc != VK_SUCCESS) return rc;
    /* The offset must be representable and the whole layer must fit, otherwise
     * a layer-addressed target cannot be honest for this surface. */
    if (!footprint || target_layer > (UINT64_MAX - (uintptr_t)address) / footprint)
        return VK_ERROR_UNKNOWN;
    const uint64_t offset = target_layer * footprint;
    if (offset > bytes || footprint > bytes - offset) return VK_ERROR_UNKNOWN;
    uintptr_t base = (uintptr_t)address + (uintptr_t)offset;
    if (!base || base >= (UINT64_C(1) << 48) || footprint > (UINT64_C(1) << 48) - base)
        return VK_ERROR_UNKNOWN;
    struct ps5vk_target_registers result = {0};
    const uint32_t width = image->info.extent.width, height = image->info.extent.height;
    if (view->format == VK_FORMAT_D32_SFLOAT) {
        if (!(image->info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return VK_ERROR_UNKNOWN;
        if (ps5_depth_build_d32_no_htile(result.registers, base, width, height)) return VK_ERROR_UNKNOWN;
        result.count = PS5_DEPTH_REGISTER_COUNT;
    } else if (view->format == VK_FORMAT_B8G8R8A8_UNORM ||
               view->format == VK_FORMAT_R8G8B8A8_UNORM) {
        if (!(image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return VK_ERROR_UNKNOWN;
        if (ps5_color_build_target(result.registers, color_defaults, base, width, height))
            return VK_ERROR_UNKNOWN;
        if (view->format == VK_FORMAT_B8G8R8A8_UNORM)
            result.registers[2].value=(result.registers[2].value & ~UINT32_C(0x1800)) | UINT32_C(0x0800);
        result.count = PS5_COLOR_REGISTER_COUNT;
    } else {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    *out = result; return VK_SUCCESS;
}

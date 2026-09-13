#include "targets_ps5.h"
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

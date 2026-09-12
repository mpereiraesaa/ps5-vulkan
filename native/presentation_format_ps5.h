#ifndef PS5VK_PRESENTATION_FORMAT_PS5_H
#define PS5VK_PRESENTATION_FORMAT_PS5_H
#include <stdint.h>
#include <vulkan/vulkan.h>
/* Scoped native scanout mapping, RGB-channel order measured on FW12.02.
 * Zero means unsupported, not a fallback. This does not specify Vulkan WSI
 * colorspace, HDR, gamma transfer or arbitrary image-format support. */
static inline uint64_t ps5vk_native_video_format(VkFormat format)
{
    return format==VK_FORMAT_B8G8R8A8_UNORM ? UINT64_C(0x8000000000000000) : 0;
}
#endif

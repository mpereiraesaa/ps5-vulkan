#ifndef PS5VK_TEXTURE_COPY_H
#define PS5VK_TEXTURE_COPY_H
#include <vulkan/vulkan_core.h>
struct ps5vk_texture_copy {
    VkDeviceSize source_offset,destination_offset,source_pitch,destination_pitch;
    uint32_t row_bytes,rows;
};
/* Plan a single base-level 2D region. No copying or cache operations.
 * Sizes are accessible bytes from each resource base, not allocation sizes.
 * Output unchanged on failure. */
VkResult ps5vk_texture_copy_plan_for_format(VkFormat,uint32_t,uint32_t,
    VkDeviceSize,VkDeviceSize,const VkBufferImageCopy *,struct ps5vk_texture_copy *);
/* Compatibility helper for the existing RGBA8 transfer-only implementation. */
VkResult ps5vk_texture_copy_plan(uint32_t,uint32_t,VkDeviceSize,VkDeviceSize,
    const VkBufferImageCopy *,struct ps5vk_texture_copy *);
#endif

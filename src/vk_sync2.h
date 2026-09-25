#ifndef PS5VK_SYNC2_H
#define PS5VK_SYNC2_H

#include "vk_command.h"

/* VK_KHR_synchronization2 conversion onto the Vulkan 1.0 commands. The KHR
 * commands themselves are defined in vk_sync2.c and reachable only on a
 * device that enabled the extension and its feature. */
enum ps5vk_sync2_kind { PS5VK_SYNC2_MEMORY, PS5VK_SYNC2_BUFFER, PS5VK_SYNC2_IMAGE };
struct ps5vk_sync2_barrier {
    enum ps5vk_sync2_kind kind;
    VkPipelineStageFlags src_stage, dst_stage;
    VkAccessFlags src_access, dst_access;
    VkMemoryBarrier memory;
    VkBufferMemoryBarrier buffer;
    VkImageMemoryBarrier image;
};

VkBool32 ps5vk_sync2_stage_mask(VkPipelineStageFlags2 stage2, VkBool32 source,
                                VkPipelineStageFlags *legacy);
VkBool32 ps5vk_sync2_access_mask(VkAccessFlags2 access2, VkAccessFlags *legacy);
VkBool32 ps5vk_sync2_image_layout(VkImageLayout layout, VkImage image,
                                  VkImageAspectFlags aspect, VkImageLayout *legacy);
VkResult ps5vk_sync2_convert_dependency(const VkDependencyInfo *dependency,
    struct ps5vk_sync2_barrier **out, uint32_t *out_count);
void ps5vk_sync2_union_stages(const struct ps5vk_sync2_barrier *list, uint32_t count,
    VkPipelineStageFlags *src, VkPipelineStageFlags *dst);

#endif

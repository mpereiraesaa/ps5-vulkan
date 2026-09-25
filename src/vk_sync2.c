#include "vk_sync2.h"
#include <stdint.h>

static VkBool32 legacy_stage(VkPipelineStageFlags2 stage2,
                              VkPipelineStageFlags *legacy)
{
    const VkPipelineStageFlags2 copy = VK_PIPELINE_STAGE_2_COPY_BIT;
    if (stage2 & ~(VkPipelineStageFlags2)(UINT32_MAX | copy)) return VK_FALSE;
    *legacy = (VkPipelineStageFlags)(stage2 & UINT32_MAX);
    if (stage2 & copy) *legacy |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    return VK_TRUE;
}

/* The pinned DXVK blitter records one image transition per dependency. The
 * legacy recorder remains the authority for image role, layout, range and
 * swapchain ownership; these checks only prevent lossy stage2 conversion. */
void ps5vk_cmd_pipeline_barrier2_bounded(VkCommandBuffer command,
                                          const VkDependencyInfo *dependency)
{
    if (!dependency || dependency->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO ||
        dependency->pNext || dependency->dependencyFlags ||
        dependency->memoryBarrierCount || dependency->bufferMemoryBarrierCount ||
        dependency->imageMemoryBarrierCount != 1 ||
        !dependency->pImageMemoryBarriers) {
        ps5vk_command_invalidate(command); return;
    }
    const VkImageMemoryBarrier2 *b = dependency->pImageMemoryBarriers;
    if (b->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 || b->pNext ||
        b->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
        b->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
        b->srcAccessMask > UINT32_MAX || b->dstAccessMask > UINT32_MAX ||
        (!b->srcStageMask && (b->srcAccessMask ||
             b->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED)) || !b->dstStageMask) {
        ps5vk_command_invalidate(command); return;
    }
    VkPipelineStageFlags src, dst;
    if (!legacy_stage(b->srcStageMask, &src) ||
        !legacy_stage(b->dstStageMask, &dst)) {
        ps5vk_command_invalidate(command); return;
    }
    if (!src) src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const VkImageMemoryBarrier legacy = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = (VkAccessFlags)b->srcAccessMask,
        .dstAccessMask = (VkAccessFlags)b->dstAccessMask,
        .oldLayout = b->oldLayout, .newLayout = b->newLayout,
        .srcQueueFamilyIndex = b->srcQueueFamilyIndex,
        .dstQueueFamilyIndex = b->dstQueueFamilyIndex,
        .image = b->image, .subresourceRange = b->subresourceRange};
    vkCmdPipelineBarrier(command, src, dst, 0, 0, NULL, 0, NULL, 1, &legacy);
}

void ps5vk_cmd_copy_buffer_to_image2_bounded(VkCommandBuffer command,
                                              const VkCopyBufferToImageInfo2 *copy)
{
    if (!copy || copy->sType != VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2 ||
        copy->pNext || copy->regionCount != 1 || !copy->pRegions) {
        ps5vk_command_invalidate(command); return;
    }
    const VkBufferImageCopy2 *r = copy->pRegions;
    if (r->sType != VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 || r->pNext) {
        ps5vk_command_invalidate(command); return;
    }
    const VkBufferImageCopy legacy = {
        .bufferOffset = r->bufferOffset, .bufferRowLength = r->bufferRowLength,
        .bufferImageHeight = r->bufferImageHeight,
        .imageSubresource = r->imageSubresource, .imageOffset = r->imageOffset,
        .imageExtent = r->imageExtent};
    vkCmdCopyBufferToImage(command, copy->srcBuffer, copy->dstImage,
        copy->dstImageLayout, 1, &legacy);
}

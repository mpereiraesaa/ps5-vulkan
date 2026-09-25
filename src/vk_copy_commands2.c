/* VK_KHR_copy_commands2 (DXVK262-T10).
 *
 * The six version-2 transfer commands add only a structure wrapper and a pNext
 * slot per region; no pinned structure extends them on this device (the
 * transform and cubic-weight structures belong to extensions it does not
 * expose), so every chain must be empty. Each command checks the structure
 * types and the chains, converts its regions into the version-1 array and
 * calls the version-1 command, which then applies exactly the rules and the
 * limits it always applies: the same accepted image roles and layouts, the
 * same region planning, the same one-operation-per-region recording and the
 * same bound. Nothing is recorded that the version-1 command would not record.
 *
 * The pinned DXVK 2.6.2 reads a staging texture back with one
 * vkCmdCopyImageToBuffer2 region (dxvk_context.cpp:3611,3664-3671) and uploads
 * textures with one vkCmdCopyBufferToImage2 region; the other four commands
 * are part of the extension and follow the same conversion.
 *
 * A refused call poisons the recording like any other refused command and
 * records no operation. */
#include "vk_command.h"
#include <string.h>

/* The version-1 commands reserve one operation per region, so a call with
 * more regions than a command buffer can hold is refused by them anyway; the
 * conversion arrays are bounded by the same number. */
enum { COPY2_MAX_REGIONS = PS5VK_MAX_OPERATIONS };

static int copy2_recording(VkCommandBuffer c)
{
    if (!c) return 0;
    if (c->state != PS5VK_RECORDING || !c->pool || !c->pool->device ||
        !c->pool->device->copy_commands2_extension_enabled) {
        ps5vk_command_invalidate(c);
        return 0;
    }
    return 1;
}
static int regions_valid(uint32_t count, const void *regions)
{
    return count && regions && count <= COPY2_MAX_REGIONS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2KHR(VkCommandBuffer c, const VkCopyBufferInfo2 *info)
{
    if (!copy2_recording(c)) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2 || info->pNext ||
        !regions_valid(info->regionCount, info->pRegions)) {
        ps5vk_command_invalidate(c); return;
    }
    VkBufferCopy regions[COPY2_MAX_REGIONS];
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const VkBufferCopy2 *r = &info->pRegions[i];
        if (r->sType != VK_STRUCTURE_TYPE_BUFFER_COPY_2 || r->pNext) {
            ps5vk_command_invalidate(c); return;
        }
        regions[i] = (VkBufferCopy){r->srcOffset, r->dstOffset, r->size};
    }
    vkCmdCopyBuffer(c, info->srcBuffer, info->dstBuffer, info->regionCount, regions);
}

static int buffer_image_regions(VkCommandBuffer c, uint32_t count,
    const VkBufferImageCopy2 *in, VkBufferImageCopy *out)
{
    for (uint32_t i = 0; i < count; ++i) {
        const VkBufferImageCopy2 *r = &in[i];
        if (r->sType != VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 || r->pNext) {
            ps5vk_command_invalidate(c); return 0;
        }
        out[i] = (VkBufferImageCopy){r->bufferOffset, r->bufferRowLength,
            r->bufferImageHeight, r->imageSubresource, r->imageOffset, r->imageExtent};
    }
    return 1;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2KHR(VkCommandBuffer c,
    const VkCopyBufferToImageInfo2 *info)
{
    if (!copy2_recording(c)) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2 || info->pNext ||
        !regions_valid(info->regionCount, info->pRegions)) {
        ps5vk_command_invalidate(c); return;
    }
    VkBufferImageCopy regions[COPY2_MAX_REGIONS];
    if (!buffer_image_regions(c, info->regionCount, info->pRegions, regions)) return;
    vkCmdCopyBufferToImage(c, info->srcBuffer, info->dstImage, info->dstImageLayout,
        info->regionCount, regions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2KHR(VkCommandBuffer c,
    const VkCopyImageToBufferInfo2 *info)
{
    if (!copy2_recording(c)) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2 || info->pNext ||
        !regions_valid(info->regionCount, info->pRegions)) {
        ps5vk_command_invalidate(c); return;
    }
    VkBufferImageCopy regions[COPY2_MAX_REGIONS];
    if (!buffer_image_regions(c, info->regionCount, info->pRegions, regions)) return;
    vkCmdCopyImageToBuffer(c, info->srcImage, info->srcImageLayout, info->dstBuffer,
        info->regionCount, regions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2KHR(VkCommandBuffer c, const VkCopyImageInfo2 *info)
{
    if (!copy2_recording(c)) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 || info->pNext ||
        !regions_valid(info->regionCount, info->pRegions)) {
        ps5vk_command_invalidate(c); return;
    }
    VkImageCopy regions[COPY2_MAX_REGIONS];
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const VkImageCopy2 *r = &info->pRegions[i];
        if (r->sType != VK_STRUCTURE_TYPE_IMAGE_COPY_2 || r->pNext) {
            ps5vk_command_invalidate(c); return;
        }
        regions[i] = (VkImageCopy){r->srcSubresource, r->srcOffset, r->dstSubresource,
            r->dstOffset, r->extent};
    }
    vkCmdCopyImage(c, info->srcImage, info->srcImageLayout, info->dstImage,
        info->dstImageLayout, info->regionCount, regions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2KHR(VkCommandBuffer c, const VkBlitImageInfo2 *info)
{
    if (!copy2_recording(c)) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2 || info->pNext ||
        !regions_valid(info->regionCount, info->pRegions)) {
        ps5vk_command_invalidate(c); return;
    }
    VkImageBlit regions[COPY2_MAX_REGIONS];
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const VkImageBlit2 *r = &info->pRegions[i];
        if (r->sType != VK_STRUCTURE_TYPE_IMAGE_BLIT_2 || r->pNext) {
            ps5vk_command_invalidate(c); return;
        }
        regions[i].srcSubresource = r->srcSubresource;
        regions[i].dstSubresource = r->dstSubresource;
        memcpy(regions[i].srcOffsets, r->srcOffsets, sizeof(regions[i].srcOffsets));
        memcpy(regions[i].dstOffsets, r->dstOffsets, sizeof(regions[i].dstOffsets));
    }
    vkCmdBlitImage(c, info->srcImage, info->srcImageLayout, info->dstImage,
        info->dstImageLayout, info->regionCount, regions, info->filter);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2KHR(VkCommandBuffer c,
    const VkResolveImageInfo2 *info)
{
    if (!copy2_recording(c)) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2 || info->pNext ||
        !regions_valid(info->regionCount, info->pRegions)) {
        ps5vk_command_invalidate(c); return;
    }
    VkImageResolve regions[COPY2_MAX_REGIONS];
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const VkImageResolve2 *r = &info->pRegions[i];
        if (r->sType != VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2 || r->pNext) {
            ps5vk_command_invalidate(c); return;
        }
        regions[i] = (VkImageResolve){r->srcSubresource, r->srcOffset, r->dstSubresource,
            r->dstOffset, r->extent};
    }
    vkCmdResolveImage(c, info->srcImage, info->srcImageLayout, info->dstImage,
        info->dstImageLayout, info->regionCount, regions);
}

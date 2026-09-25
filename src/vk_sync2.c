#include "vk_sync2.h"
#include "vk_image.h"
#include "vk_queue.h"
#include <stdint.h>
#include <stdlib.h>

/* VK_KHR_synchronization2 on the Vulkan 1.0 profile. Every command converts
 * its synchronization2 structures into the existing Vulkan 1.0 command, which
 * stays the authority for image roles, layouts, ranges and queue ownership.
 * The conversion itself only widens scopes (the 1.0 equivalent of a stage2 or
 * access2 bit covers at least what the bit names) and refuses every bit or
 * structure it cannot represent, before anything is recorded. */

#define SYNC2_CORE_STAGES ((VkPipelineStageFlags2)0x1FFFFu)   /* TOP..ALL_COMMANDS */
#define SYNC2_CORE_ACCESS ((VkAccessFlags2)0x1FFFFu)          /* INDIRECT..MEMORY_WRITE */

VkBool32 ps5vk_sync2_stage_mask(VkPipelineStageFlags2 stage2, VkBool32 source,
                                VkPipelineStageFlags *legacy)
{
    const VkPipelineStageFlags2 transfer = VK_PIPELINE_STAGE_2_COPY_BIT |
        VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
        VK_PIPELINE_STAGE_2_CLEAR_BIT;
    const VkPipelineStageFlags2 vertex_input = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    const VkPipelineStageFlags2 pre_raster =
        VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
    if (stage2 & ~(SYNC2_CORE_STAGES | transfer | vertex_input | pre_raster))
        return VK_FALSE;
    VkPipelineStageFlags out = (VkPipelineStageFlags)(stage2 & SYNC2_CORE_STAGES);
    if (stage2 & transfer) out |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (stage2 & vertex_input) out |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (stage2 & pre_raster)
        out |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
            VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
            VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    /* NONE: no stage in the first scope is TOP_OF_PIPE, in the second
     * BOTTOM_OF_PIPE; Vulkan 1.0 has no empty stage mask. */
    if (!out) out = source ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT :
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    *legacy = out;
    return VK_TRUE;
}

VkBool32 ps5vk_sync2_access_mask(VkAccessFlags2 access2, VkAccessFlags *legacy)
{
    const VkAccessFlags2 read = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    const VkAccessFlags2 write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (access2 & ~(SYNC2_CORE_ACCESS | read | write)) return VK_FALSE;
    VkAccessFlags out = (VkAccessFlags)(access2 & SYNC2_CORE_ACCESS);
    if (access2 & read) out |= VK_ACCESS_SHADER_READ_BIT;
    if (access2 & write) out |= VK_ACCESS_SHADER_WRITE_BIT;
    *legacy = out;
    return VK_TRUE;
}

static VkBool32 combined_depth_stencil(VkFormat format)
{
    return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
        format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

/* ATTACHMENT_OPTIMAL and READ_ONLY_OPTIMAL name the attachment or read-only
 * layout of whatever aspect the barrier covers. They resolve to the concrete
 * Vulkan 1.0 layout only when that is unambiguous: a colour range, or a
 * depth/stencil range covering every aspect of its format. A single aspect of
 * a combined format would need the separate depth/stencil layouts. */
VkBool32 ps5vk_sync2_image_layout(VkImageLayout layout, VkImage image,
                                  VkImageAspectFlags aspect, VkImageLayout *legacy)
{
    if (layout != VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL &&
        layout != VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL) {
        *legacy = layout; return VK_TRUE;
    }
    const VkBool32 attachment = layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    const VkImageAspectFlags ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        *legacy = attachment ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return VK_TRUE;
    }
    if (!image || !aspect || (aspect & ~ds) ||
        (aspect != ds && combined_depth_stencil(image->info.format)))
        return VK_FALSE;
    *legacy = attachment ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    return VK_TRUE;
}

static VkBool32 sync2_enabled(VkCommandBuffer command)
{
    return command && command->pool && command->pool->device &&
        (command->pool->device->enabled_features_t09 & PS5VK_T09_FEATURE_SYNCHRONIZATION2);
}

static VkBool32 scope_pair(VkPipelineStageFlags2 src_stage2, VkAccessFlags2 src_access2,
    VkPipelineStageFlags2 dst_stage2, VkAccessFlags2 dst_access2,
    struct ps5vk_sync2_barrier *out)
{
    /* An access in an empty (NONE) stage scope names nothing to order. */
    if ((!src_stage2 && src_access2) || (!dst_stage2 && dst_access2)) return VK_FALSE;
    return ps5vk_sync2_stage_mask(src_stage2, VK_TRUE, &out->src_stage) &&
        ps5vk_sync2_stage_mask(dst_stage2, VK_FALSE, &out->dst_stage) &&
        ps5vk_sync2_access_mask(src_access2, &out->src_access) &&
        ps5vk_sync2_access_mask(dst_access2, &out->dst_access);
}

/* Converts every member of one VkDependencyInfo, or none. The caller owns
 * the returned array (memory, then buffer, then image members). */
VkResult ps5vk_sync2_convert_dependency(const VkDependencyInfo *dependency,
    struct ps5vk_sync2_barrier **out, uint32_t *out_count)
{
    *out = NULL; *out_count = 0;
    if (!dependency || dependency->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO ||
        dependency->pNext ||
        (dependency->memoryBarrierCount && !dependency->pMemoryBarriers) ||
        (dependency->bufferMemoryBarrierCount && !dependency->pBufferMemoryBarriers) ||
        (dependency->imageMemoryBarrierCount && !dependency->pImageMemoryBarriers))
        return VK_ERROR_UNKNOWN;
    const uint64_t total = (uint64_t)dependency->memoryBarrierCount +
        dependency->bufferMemoryBarrierCount + dependency->imageMemoryBarrierCount;
    if (total > PS5VK_MAX_OPERATIONS) return VK_ERROR_UNKNOWN;
    if (!total) return VK_SUCCESS;
    struct ps5vk_sync2_barrier *list = calloc((size_t)total, sizeof(*list));
    if (!list) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t n = 0;
    for (uint32_t j = 0; j < dependency->memoryBarrierCount; ++j, ++n) {
        const VkMemoryBarrier2 *b = &dependency->pMemoryBarriers[j];
        struct ps5vk_sync2_barrier *dst = &list[n];
        dst->kind = PS5VK_SYNC2_MEMORY;
        if (b->sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 || b->pNext ||
            !scope_pair(b->srcStageMask, b->srcAccessMask, b->dstStageMask,
                        b->dstAccessMask, dst)) goto refuse;
        dst->memory = (VkMemoryBarrier){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = dst->src_access, .dstAccessMask = dst->dst_access};
    }
    for (uint32_t j = 0; j < dependency->bufferMemoryBarrierCount; ++j, ++n) {
        const VkBufferMemoryBarrier2 *b = &dependency->pBufferMemoryBarriers[j];
        struct ps5vk_sync2_barrier *dst = &list[n];
        dst->kind = PS5VK_SYNC2_BUFFER;
        if (b->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || b->pNext ||
            !scope_pair(b->srcStageMask, b->srcAccessMask, b->dstStageMask,
                        b->dstAccessMask, dst)) goto refuse;
        dst->buffer = (VkBufferMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = dst->src_access, .dstAccessMask = dst->dst_access,
            .srcQueueFamilyIndex = b->srcQueueFamilyIndex,
            .dstQueueFamilyIndex = b->dstQueueFamilyIndex,
            .buffer = b->buffer, .offset = b->offset, .size = b->size};
    }
    for (uint32_t j = 0; j < dependency->imageMemoryBarrierCount; ++j, ++n) {
        const VkImageMemoryBarrier2 *b = &dependency->pImageMemoryBarriers[j];
        struct ps5vk_sync2_barrier *dst = &list[n];
        VkImageLayout old_layout, new_layout;
        dst->kind = PS5VK_SYNC2_IMAGE;
        if (b->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 || b->pNext ||
            !scope_pair(b->srcStageMask, b->srcAccessMask, b->dstStageMask,
                        b->dstAccessMask, dst) ||
            !ps5vk_sync2_image_layout(b->oldLayout, b->image,
                b->subresourceRange.aspectMask, &old_layout) ||
            !ps5vk_sync2_image_layout(b->newLayout, b->image,
                b->subresourceRange.aspectMask, &new_layout)) goto refuse;
        dst->image = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = dst->src_access, .dstAccessMask = dst->dst_access,
            .oldLayout = old_layout, .newLayout = new_layout,
            .srcQueueFamilyIndex = b->srcQueueFamilyIndex,
            .dstQueueFamilyIndex = b->dstQueueFamilyIndex,
            .image = b->image, .subresourceRange = b->subresourceRange};
    }
    *out = list; *out_count = n;
    return VK_SUCCESS;
refuse:
    free(list);
    return VK_ERROR_UNKNOWN;
}

/* Records one converted dependency. Members of one VkDependencyInfo are
 * unordered with respect to each other, so members that share a stage pair
 * go into one Vulkan 1.0 barrier, and each distinct pair gets its own barrier
 * in order of first appearance. Separate barriers are at least as strong as
 * the single synchronization2 dependency. */
static void record_barriers(VkCommandBuffer command, VkDependencyFlags flags,
    const struct ps5vk_sync2_barrier *list, uint32_t count)
{
    VkMemoryBarrier *memory = calloc(count ? count : 1, sizeof(*memory));
    VkBufferMemoryBarrier *buffers = calloc(count ? count : 1, sizeof(*buffers));
    VkImageMemoryBarrier *images = calloc(count ? count : 1, sizeof(*images));
    unsigned char *done = calloc(count ? count : 1, 1);
    if (!memory || !buffers || !images || !done) {
        ps5vk_command_invalidate(command); goto out;
    }
    for (uint32_t first = 0; first < count; ++first) {
        if (done[first]) continue;
        const VkPipelineStageFlags src = list[first].src_stage;
        const VkPipelineStageFlags dst = list[first].dst_stage;
        uint32_t nm = 0, nb = 0, ni = 0;
        for (uint32_t j = first; j < count; ++j) {
            if (done[j] || list[j].src_stage != src || list[j].dst_stage != dst) continue;
            done[j] = 1;
            if (list[j].kind == PS5VK_SYNC2_MEMORY) memory[nm++] = list[j].memory;
            else if (list[j].kind == PS5VK_SYNC2_BUFFER) buffers[nb++] = list[j].buffer;
            else images[ni++] = list[j].image;
        }
        vkCmdPipelineBarrier(command, src, dst, flags, nm, memory, nb, buffers, ni, images);
        if (command->state != PS5VK_RECORDING) goto out;
    }
out:
    free(memory); free(buffers); free(images); free(done);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2KHR(VkCommandBuffer command,
    const VkDependencyInfo *dependency)
{
    if (!sync2_enabled(command)) { ps5vk_command_invalidate(command); return; }
    struct ps5vk_sync2_barrier *list; uint32_t count;
    if (ps5vk_sync2_convert_dependency(dependency, &list, &count) != VK_SUCCESS) {
        ps5vk_command_invalidate(command); return;
    }
    if (!count) {
        /* A dependency without members is an execution dependency between
         * everything before and everything after: keep it one. */
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dependency->dependencyFlags,
            0, NULL, 0, NULL, 0, NULL);
        return;
    }
    record_barriers(command, dependency->dependencyFlags, list, count);
    free(list);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2KHR(VkCommandBuffer command, VkEvent event,
    const VkDependencyInfo *dependency)
{
    struct ps5vk_sync2_barrier *list; uint32_t count;
    /* The memory scope of a set/wait pair is carried by the matching
     * vkCmdWaitEvents2, whose barriers the Vulkan 1.0 wait records; the set
     * keeps the union of the source stages. */
    if (!sync2_enabled(command) || !dependency || dependency->dependencyFlags ||
        ps5vk_sync2_convert_dependency(dependency, &list, &count) != VK_SUCCESS) {
        ps5vk_command_invalidate(command); return;
    }
    VkPipelineStageFlags stages = 0;
    for (uint32_t j = 0; j < count; ++j) stages |= list[j].src_stage;
    free(list);
    vkCmdSetEvent(command, event, stages ? stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2KHR(VkCommandBuffer command, VkEvent event,
    VkPipelineStageFlags2 stage2)
{
    VkPipelineStageFlags stage;
    if (!sync2_enabled(command) || !ps5vk_sync2_stage_mask(stage2, VK_TRUE, &stage)) {
        ps5vk_command_invalidate(command); return;
    }
    vkCmdResetEvent(command, event, stage);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2KHR(VkCommandBuffer command, uint32_t count,
    const VkEvent *events, const VkDependencyInfo *dependencies)
{
    if (!sync2_enabled(command) || !count || !events || !dependencies ||
        count > PS5VK_MAX_OPERATIONS) {
        ps5vk_command_invalidate(command); return;
    }
    /* Vulkan 1.0 waits once for every event with the union of the stages the
     * events were set with and the union of the destination stages; each
     * member keeps its own access masks inside that wider scope. */
    struct ps5vk_sync2_barrier *all = NULL; uint32_t total = 0;
    VkPipelineStageFlags src = 0, dst = 0;
    VkMemoryBarrier *memory = NULL; VkBufferMemoryBarrier *buffers = NULL;
    VkImageMemoryBarrier *images = NULL;
    uint32_t nm = 0, nb = 0, ni = 0;
    for (uint32_t e = 0; e < count; ++e) {
        struct ps5vk_sync2_barrier *list; uint32_t n;
        if (dependencies[e].dependencyFlags ||
            ps5vk_sync2_convert_dependency(&dependencies[e], &list, &n) != VK_SUCCESS)
            goto refuse;
        if (!n) continue;
        if ((uint64_t)total + n > PS5VK_MAX_OPERATIONS) { free(list); goto refuse; }
        struct ps5vk_sync2_barrier *grown = realloc(all, (size_t)(total + n) * sizeof(*all));
        if (!grown) { free(list); goto refuse; }
        all = grown;
        for (uint32_t j = 0; j < n; ++j) all[total + j] = list[j];
        total += n; free(list);
    }
    memory = calloc(total ? total : 1, sizeof(*memory));
    buffers = calloc(total ? total : 1, sizeof(*buffers));
    images = calloc(total ? total : 1, sizeof(*images));
    if (!memory || !buffers || !images) goto refuse;
    for (uint32_t j = 0; j < total; ++j) {
        src |= all[j].src_stage; dst |= all[j].dst_stage;
        if (all[j].kind == PS5VK_SYNC2_MEMORY) memory[nm++] = all[j].memory;
        else if (all[j].kind == PS5VK_SYNC2_BUFFER) buffers[nb++] = all[j].buffer;
        else images[ni++] = all[j].image;
    }
    if (!src) src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (!dst) dst = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    vkCmdWaitEvents(command, count, events, src, dst, nm, memory, nb, buffers, ni, images);
    free(all); free(memory); free(buffers); free(images);
    return;
refuse:
    free(all); free(memory); free(buffers); free(images);
    ps5vk_command_invalidate(command);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2KHR(VkCommandBuffer command,
    VkPipelineStageFlags2 stage2, VkQueryPool pool, uint32_t query)
{
    VkPipelineStageFlags stage;
    /* One stage bit that has a single Vulkan 1.0 bit of its own. */
    if (!sync2_enabled(command) || !stage2 || (stage2 & (stage2 - 1)) ||
        (stage2 & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) ||
        !ps5vk_sync2_stage_mask(stage2, VK_TRUE, &stage)) {
        ps5vk_command_invalidate(command); return;
    }
    vkCmdWriteTimestamp(command, (VkPipelineStageFlagBits)stage, pool, query);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue queue, uint32_t count,
    const VkSubmitInfo2 *submits, VkFence fence)
{
    if (!queue || !queue->device ||
        !(queue->device->enabled_features_t09 & PS5VK_T09_FEATURE_SYNCHRONIZATION2))
        return VK_ERROR_UNKNOWN;
    return ps5vk_queue_submit2_bounded(queue, count, submits, fence);
}

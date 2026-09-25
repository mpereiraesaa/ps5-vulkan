#include "vk_sync2.h"
#include "vk_image.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static VkResult allocate(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx;
    *address = *backing = calloc(1, (size_t)size);
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void release(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult sync_memory(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult image_requirements(VkDevice d, const VkImageCreateInfo *info,
                                   VkMemoryRequirements *out)
{
    (void)d;
    if (info->format != VK_FORMAT_B8G8R8A8_UNORM ||
        info->extent.width != 16 || info->extent.height != 16)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out = (VkMemoryRequirements){4096, 256, 1};
    return VK_SUCCESS;
}
static VkDeviceMemory memory(VkDevice d, VkDeviceSize size)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = size, .memoryTypeIndex = 0};
    VkDeviceMemory out = VK_NULL_HANDLE;
    assert(vkAllocateMemory(d, &info, NULL, &out) == VK_SUCCESS);
    return out;
}
static void recording(struct VkCommandBuffer_T *command, VkCommandPool pool)
{ *command = (struct VkCommandBuffer_T){.pool = pool, .state = PS5VK_RECORDING}; }

int main(void)
{
    struct VkDevice_T d = {.graphics_enabled = VK_TRUE,
        .max_allocation = 1u << 20, .buffer_alignment = 16,
        .noncoherent_atom = 1, .image_requirements = image_requirements,
        .memory = {.allocate = allocate, .release = release,
                   .flush = sync_memory, .invalidate = sync_memory}};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image = VK_NULL_HANDLE;
    assert(vkCreateImage(&d, &ii, NULL, &image) == VK_SUCCESS);
    VkDeviceMemory image_memory = memory(&d, 4096);
    assert(vkBindImageMemory(&d, image, image_memory, 0) == VK_SUCCESS);
    image->swapchain_owned = VK_TRUE;

    struct VkCommandPool_T pool = {.device = &d};
    struct VkCommandBuffer_T command;
    recording(&command, &pool);
    VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    ps5vk_cmd_pipeline_barrier2_bounded(&command, &dependency);
    assert(command.state == PS5VK_RECORDING && command.operation_count == 1);
    assert(command.operations[0].type == PS5VK_IMAGE_BARRIER &&
        command.operations[0].src_stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT &&
        command.operations[0].dst_stage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    ps5vk_cmd_pipeline_barrier2_bounded(&command, &dependency);
    assert(command.state == PS5VK_RECORDING && command.operation_count == 2);
    assert(command.operations[1].image_barrier.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
        command.operations[1].dst_access == VK_ACCESS_MEMORY_READ_BIT);

    recording(&command, &pool);
    image->swapchain_owned = VK_FALSE;
    ps5vk_cmd_pipeline_barrier2_bounded(&command, &dependency);
    assert(command.state == PS5VK_INVALID && !command.operation_count);
    image->swapchain_owned = VK_TRUE;

    recording(&command, &pool);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_RESOLVE_BIT;
    ps5vk_cmd_pipeline_barrier2_bounded(&command, &dependency);
    assert(command.state == PS5VK_INVALID && !command.operation_count);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    recording(&command, &pool);
    dependency.bufferMemoryBarrierCount = 1;
    ps5vk_cmd_pipeline_barrier2_bounded(&command, &dependency);
    assert(command.state == PS5VK_INVALID && !command.operation_count);
    dependency.bufferMemoryBarrierCount = 0;

    recording(&command, &pool);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ps5vk_cmd_pipeline_barrier2_bounded(&command, &dependency);
    assert(command.state == PS5VK_RECORDING && command.operation_count == 1 &&
        command.operations[0].dst_stage == VK_PIPELINE_STAGE_TRANSFER_BIT);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ps5vk_cmd_pipeline_barrier2_bounded(&command, &dependency);
    assert(command.state == PS5VK_RECORDING && command.operation_count == 2 &&
        command.operations[1].src_stage == VK_PIPELINE_STAGE_TRANSFER_BIT);

    recording(&command, &pool);
    dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    ps5vk_cmd_pipeline_barrier2_bounded(&command, &dependency);
    assert(command.state == PS5VK_INVALID && !command.operation_count);
    dependency.dependencyFlags = 0;

    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 16u * 16u * 4u, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer = VK_NULL_HANDLE;
    assert(vkCreateBuffer(&d, &bi, NULL, &buffer) == VK_SUCCESS);
    VkDeviceMemory buffer_memory = memory(&d, bi.size);
    assert(vkBindBufferMemory(&d, buffer, buffer_memory, 0) == VK_SUCCESS);
    VkBufferImageCopy2 region = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {16, 16, 1}};
    VkCopyBufferToImageInfo2 copy = {.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = buffer, .dstImage = image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1, .pRegions = &region};
    recording(&command, &pool);
    ps5vk_cmd_copy_buffer_to_image2_bounded(&command, &copy);
    assert(command.state == PS5VK_RECORDING && command.operation_count == 1 &&
        command.operations[0].type == PS5VK_COPY_BUFFER_IMAGE &&
        command.operations[0].copy_region.imageExtent.width == 16);

    recording(&command, &pool);
    copy.regionCount = 2;
    ps5vk_cmd_copy_buffer_to_image2_bounded(&command, &copy);
    assert(command.state == PS5VK_INVALID && !command.operation_count);

    vkDestroyBuffer(&d, buffer, NULL);
    vkFreeMemory(&d, buffer_memory, NULL);
    vkDestroyImage(&d, image, NULL);
    vkFreeMemory(&d, image_memory, NULL);
    assert(!d.buffers && !d.images && !d.memories);
    puts("Bounded synchronization2 image barrier/copy: pass (host recording only)");
}

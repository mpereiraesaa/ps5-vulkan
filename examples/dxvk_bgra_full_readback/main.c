/* SPDX-License-Identifier: GPL-3.0-or-later
 * Full 1920x1080 BGRA8 readback through the public Vulkan SDK. This is the
 * bounded image-to-buffer transfer shape used by DXVK's D3D8/9 backbuffer
 * readback, with a different exact clear colour on each of two submissions.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 1920, HEIGHT = 1080, BYTES = WIDTH * HEIGHT * 4,
       PREFIX = 4096, GUARD = 64,
       FRAMES = 2 };
static const uint64_t fence_timeout = UINT64_C(300000000);

static uint32_t digest(const uint8_t *data)
{
    uint32_t value = UINT32_C(2166136261);
    for (size_t i = 0; i < BYTES; ++i)
        value = (value ^ data[i]) * UINT32_C(16777619);
    return value;
}

static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(test, reason) do { if (!(test)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer buffers[FRAMES] = {VK_NULL_HANDLE};
    VkDeviceMemory memories[FRAMES] = {VK_NULL_HANDLE};
    uint8_t *mapped[FRAMES] = {NULL};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[FRAMES] = {VK_NULL_HANDLE};
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 pending = VK_FALSE;
    unsigned passed = 0;

    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t physical_count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &physical_count, &physical));
    REQUIRE(physical_count == 1 && physical, "one physical device");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    const char *extension = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "graphics queue");

    const VkFormat formats[2] = {VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_FORMAT_B8G8R8A8_SRGB};
    VkImageFormatListCreateInfo format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = formats};
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &format_list, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = formats[0],
        .extent = {WIDTH, HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    TRY(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &allocation, NULL, &image_memory));
    TRY(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = formats[0],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    TRY(vkCreateImageView(device, &view_info, NULL, &view));

    VkAttachmentDescription attachment = {.format = formats[0],
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass};
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &view,
        .width = WIDTH, .height = HEIGHT, .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = PREFIX + BYTES + GUARD,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        TRY(vkCreateBuffer(device, &buffer_info, NULL, &buffers[frame]));
        vkGetBufferMemoryRequirements(device, buffers[frame], &requirements);
        allocation.allocationSize = requirements.size;
        TRY(vkAllocateMemory(device, &allocation, NULL, &memories[frame]));
        TRY(vkBindBufferMemory(device, buffers[frame], memories[frame], 0));
        TRY(vkMapMemory(device, memories[frame], 0, VK_WHOLE_SIZE, 0,
                        (void **)&mapped[frame]));
        memset(mapped[frame], 0xcd, PREFIX + BYTES + GUARD);
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[frame], .size = VK_WHOLE_SIZE};
        TRY(vkFlushMappedMemoryRanges(device, 1, &range));
    }

    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0};
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES};
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    ps5log_printf(PS5LOG_MARK,
        "DXVK_BGRA_FULL_READBACK_START width=%u height=%u bytes=%u offset=%u usage=%u frames=%u",
        WIDTH, HEIGHT, BYTES, PREFIX, (unsigned)image_info.usage, FRAMES);

    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        VkCommandBuffer command = commands[frame];
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(command, &begin));
        /* Each new clear discards the preceding readback image contents.
         * DXVK's first-use BGRA8 handover uses COLOR_OUTPUT on both sides. */
        VkImageMemoryBarrier first_use = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, NULL, 0, NULL, 1, &first_use);
        VkClearValue clear = {.color = {.float32 = {frame ? 0.0f : 1.0f,
            frame ? 1.0f : 0.0f, 0.0f, 1.0f}}};
        VkRenderPassBeginInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass, .framebuffer = framebuffer,
            .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
            .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(command);
        VkImageMemoryBarrier image_barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image_barrier);
        VkBufferImageCopy copy = {.bufferOffset = PREFIX,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {WIDTH, HEIGHT, 1}};
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffers[frame], 1, &copy);
        VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffers[frame], .size = VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        TRY(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        pending = VK_TRUE;
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
        pending = VK_FALSE;
        TRY(vkResetFences(device, 1, &fence));
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[frame], .size = VK_WHOLE_SIZE};
        TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
        const uint8_t expected[4] = {0, frame ? 255 : 0, frame ? 0 : 255, 255};
        unsigned mismatches = 0, prefix_mismatches = 0, guard_mismatches = 0;
        for (size_t offset = 0; offset < PREFIX; ++offset)
            prefix_mismatches += mapped[frame][offset] != 0xcd;
        for (size_t offset = 0; offset < BYTES; offset += 4)
            for (unsigned component = 0; component < 4; ++component)
                mismatches += mapped[frame][PREFIX + offset + component] != expected[component];
        for (size_t offset = PREFIX + BYTES; offset < PREFIX + BYTES + GUARD; ++offset)
            guard_mismatches += mapped[frame][offset] != 0xcd;
        ps5log_printf(PS5LOG_MARK,
            "DXVK_BGRA_FULL_READBACK_FRAME frame=%u mismatches=%u prefix=%u guard=%u digest=%08x fence=complete",
            frame, mismatches, prefix_mismatches, guard_mismatches,
            digest(mapped[frame] + PREFIX));
        REQUIRE(!mismatches && !prefix_mismatches && !guard_mismatches,
                "exact BGRA pixels, prefix and guard");
        ++passed;
    }
    ps5log_printf(PS5LOG_MARK, "DXVK_BGRA_FULL_READBACK_RESULT frames=%u passed=%u",
                  FRAMES, passed);

cleanup:
    if (pending && device && vkDeviceWaitIdle(device) != VK_SUCCESS) {
        ps5log_printf(PS5LOG_ERR,
            "DXVK_BGRA_FULL_READBACK_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "idle", (int)result);
        return 1;
    }
    if (fence) vkDestroyFence(device, fence, NULL);
    if (commands[0]) vkFreeCommandBuffers(device, pool, FRAMES, commands);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        if (mapped[frame]) vkUnmapMemory(device, memories[frame]);
        if (buffers[frame]) vkDestroyBuffer(device, buffers[frame], NULL);
        if (memories[frame]) vkFreeMemory(device, memories[frame], NULL);
    }
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    if (view) vkDestroyImageView(device, view, NULL);
    if (image) vkDestroyImage(device, image, NULL);
    if (image_memory) vkFreeMemory(device, image_memory, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "DXVK_BGRA_FULL_READBACK_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR,
            "DXVK_BGRA_FULL_READBACK_FAILURE call=%s result=%d retirement=attempted",
            failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
#undef TRY
#undef REQUIRE
}

int main(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t boot = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
    ps5log_config config;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if (ps5log_load_config(paths, 1, &config, &loaded)) _exit(1);
    config.udp = 0;
    if (ps5log_init(&config, "PPSA99994", "ps5vk", boot)) _exit(1);
    int failed = run_witness();
    ps5log_close(failed ? "dxvk-bgra-full-readback-failed" :
                          "dxvk-bgra-full-readback-end");
    for (;;) sleep(1);
}

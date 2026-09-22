/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sample_rate_probe.h"
#include "color_clear.h"
#include "ps5log.h"
#include <string.h>

/* Distinct words the scan reports before it stops distinguishing: a whole
 * surface clear must produce exactly one, and anything above this bound is
 * already a failure. */
enum { PROBE_DISTINCT_LIMIT = 8 };

VkResult ps5vk_sample_rate_probe(VkDevice device,
    const struct ps5vk_sample_rate_probe_params *params)
{
    VkResult rc = VK_ERROR_UNKNOWN;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *mapped = NULL;
    uint32_t expected = 0, first = 0, last = 0;
    unsigned distinct = 0, words = 0, correct = 0;
    uint32_t seen[PROBE_DISTINCT_LIMIT] = {0};
    VkDeviceSize bytes = 0;

#define PROBE_TRY(call) do { rc = (call); if (rc != VK_SUCCESS) { \
    ps5log_printf(PS5LOG_ERR, "PS5VK_SAMPLE_RATE_FAIL call=%s rc=%d", #call, rc); \
    goto cleanup; } } while (0)
    if (!device || !params || !params->extent || params->extent > 4096u ||
        (params->samples != VK_SAMPLE_COUNT_2_BIT &&
         params->samples != VK_SAMPLE_COUNT_4_BIT) ||
        !ps5vk_color_clear_bgra8(params->clear, &expected))
        goto cleanup;

    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {params->extent, params->extent, 1}, .mipLevels = 1,
        .arrayLayers = 1, .samples = params->samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        /* The one multisampled role this profile serves: the colour
         * attachment, and nothing else. */
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    PROBE_TRY(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    bytes = requirements.size;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    PROBE_TRY(vkAllocateMemory(device, &allocation, NULL, &memory));
    PROBE_TRY(vkBindImageMemory(device, image, memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    PROBE_TRY(vkCreateImageView(device, &view_info, NULL, &view));

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = params->samples,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass};
    PROBE_TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &view,
        .width = params->extent, .height = params->extent, .layers = 1};
    PROBE_TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
    PROBE_TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    PROBE_TRY(vkAllocateCommandBuffers(device, &allocate, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    PROBE_TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    PROBE_TRY(vkBeginCommandBuffer(command, &begin));
    VkClearValue clear = {.color = {.float32 = {params->clear[0], params->clear[1],
                                               params->clear[2], params->clear[3]}}};
    VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer,
        .renderArea = {{0, 0}, {params->extent, params->extent}},
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    /* The whole-surface clear-attachment: the in-pass work a render pass must
     * contain, and on a multisampled attachment the queue's whole-span fill. */
    VkClearAttachment clear_attachment = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0, .clearValue = clear};
    VkClearRect clear_rect = {.rect = {{0, 0}, {params->extent, params->extent}},
        .baseArrayLayer = 0, .layerCount = 1};
    vkCmdClearAttachments(command, 1, &clear_attachment, 1, &clear_rect);
    vkCmdEndRenderPass(command);
    /* No image barrier is recorded: the profile's barrier roles are the
     * transfer/readback shapes, and a colour-attachment-only multisampled
     * surface has none. The fence below is the profile's own completion
     * contract - it is observed only after the GPU's cache writeback - and the
     * readback invalidates the host view of the same memory before reading. */
    PROBE_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    PROBE_TRY(vkQueueSubmit(queue, 1, &submit, fence));
    PROBE_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    PROBE_TRY(vkResetFences(device, 1, &fence));

    PROBE_TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .offset = 0, .size = VK_WHOLE_SIZE};
    PROBE_TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
    words = (unsigned)(bytes / 4u);
    for (unsigned i = 0; i < words; ++i) {
        uint32_t word;
        memcpy(&word, (const unsigned char *)mapped + (size_t)i * 4u, sizeof(word));
        if (!i) first = word;
        last = word;
        if (word == expected) ++correct;
        unsigned found = 0;
        for (unsigned k = 0; k < distinct && !found; ++k) found = seen[k] == word;
        if (!found && distinct < PROBE_DISTINCT_LIMIT) seen[distinct++] = word;
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_SAMPLE_RATE_CLEAR extent=%ux%u samples=%u bytes=%llu words=%u "
        "expected=%08x first=%08x last=%08x distinct=%u correct=%u verdict=%u",
        params->extent, params->extent, (unsigned)params->samples,
        (unsigned long long)bytes, words, expected, first, last, distinct, correct,
        (unsigned)(distinct == 1u && correct == words && expected == first));
    rc = (distinct == 1u && correct == words && first == expected) ?
        VK_SUCCESS : VK_ERROR_UNKNOWN;

cleanup:
    if (mapped) vkUnmapMemory(device, memory);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command) vkFreeCommandBuffers(device, pool, 1, &command);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    if (view) vkDestroyImageView(device, view, NULL);
    if (image) vkDestroyImage(device, image, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    return rc;
#undef PROBE_TRY
}

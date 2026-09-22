/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "two_mrt_probe.h"
#include "two_mrt_oracle.h"
#include "ps5log.h"
#include <string.h>

enum { PROBE_EDGE = 64, PROBE_BYTES = PROBE_EDGE * PROBE_EDGE * 4u };
/* The centre of the triangle the owned runtime vertex module draws. */
enum { PROBE_PIXEL = (PROBE_EDGE / 2) * PROBE_EDGE + PROBE_EDGE / 2 };
#define PROBE_GUARD UINT32_C(0x6d6d6d6d)

VkResult ps5vk_two_mrt_probe(VkDevice device,
    const struct ps5vk_two_mrt_probe_modules *modules)
{
    VkResult rc = VK_ERROR_UNKNOWN;
    VkImage image[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory image_memory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkImage staging[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory staging_memory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    unsigned char *staging_bytes[2] = {NULL, NULL};
    VkMemoryRequirements staging_requirements[2] = {{0}, {0}};
    VkDeviceSize staging_allocation_bytes[2] = {0, 0};
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    unsigned char observed[2][4] = {{0}, {0}};

#define TRY(call) do { rc = (call); if (rc != VK_SUCCESS) { \
    ps5log_printf(PS5LOG_ERR, "PS5VK_TWO_MRT_FAIL call=%s rc=%d", #call, rc); \
    goto cleanup; } } while (0)
    if (!device || !modules || !modules->vertex || !modules->vertex_words ||
        !modules->two_mrt_fragment || !modules->two_mrt_fragment_words)
        goto cleanup;

    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 0, .memoryTypeIndex = 0};
    /* Two independent colour targets, each the exact role the pinned linear
     * readback accepts: a colour attachment that is also a transfer source and
     * destination. */
    for (unsigned i = 0; i < 2; ++i) {
        VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {PROBE_EDGE, PROBE_EDGE, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        TRY(vkCreateImage(device, &image_info, NULL, &image[i]));
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, image[i], &requirements);
        allocation.allocationSize = requirements.size;
        TRY(vkAllocateMemory(device, &allocation, NULL, &image_memory[i]));
        TRY(vkBindImageMemory(device, image[i], image_memory[i], 0));
        VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        TRY(vkCreateImageView(device, &view_info, NULL, &view[i]));
    }

    /* Both attachments are cleared to a colour neither export contains, and the
     * pass leaves them in GENERAL, which is the layout the readback path reads
     * them in (the same shape the dual-source witness uses). */
    VkAttachmentDescription attachments[2];
    for (unsigned i = 0; i < 2; ++i)
        attachments[i] = (VkAttachmentDescription){.format = VK_FORMAT_R8G8B8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference colors[2] = {
        {0, VK_IMAGE_LAYOUT_GENERAL}, {1, VK_IMAGE_LAYOUT_GENERAL}};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 2, .pColorAttachments = colors};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = attachments,
        .subpassCount = 1, .pSubpasses = &subpass};
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 2, .pAttachments = view,
        .width = PROBE_EDGE, .height = PROBE_EDGE, .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    /* One linear staging image per target, so both reads come from the same
     * single draw. */
    VkMappedMemoryRange range[2];
    for (unsigned i = 0; i < 2; ++i) {
        VkImageCreateInfo staging_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {PROBE_EDGE, PROBE_EDGE, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        TRY(vkCreateImage(device, &staging_info, NULL, &staging[i]));
        vkGetImageMemoryRequirements(device, staging[i], &staging_requirements[i]);
        if (staging_requirements[i].size > UINT64_MAX - 4096u) goto cleanup;
        staging_allocation_bytes[i] = staging_requirements[i].size + 4096u;
        allocation.allocationSize = staging_allocation_bytes[i];
        TRY(vkAllocateMemory(device, &allocation, NULL, &staging_memory[i]));
        TRY(vkBindImageMemory(device, staging[i], staging_memory[i], 0));
        TRY(vkMapMemory(device, staging_memory[i], 0, VK_WHOLE_SIZE, 0,
                        (void **)&staging_bytes[i]));
        for (VkDeviceSize offset = 0; offset + 4 <= staging_allocation_bytes[i]; offset += 4)
            memcpy(staging_bytes[i] + offset, &(uint32_t){PROBE_GUARD}, 4);
        range[i] = (VkMappedMemoryRange){.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = staging_memory[i], .offset = 0, .size = staging_allocation_bytes[i]};
        TRY(vkFlushMappedMemoryRanges(device, 1, &range[i]));
    }
    VkImageSubresourceRange staging_range = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout));
    const uint32_t *shader_words[2] = {modules->vertex, modules->two_mrt_fragment};
    const size_t shader_counts[2] = {modules->vertex_words, modules->two_mrt_fragment_words};
    for (unsigned i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo shader_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shader_counts[i] * 4u, .pCode = shader_words[i]};
        TRY(vkCreateShaderModule(device, &shader_info, NULL, &shaders[i]));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shaders[0], .pName = "main"}, {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shaders[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkViewport viewport = {0, 0, PROBE_EDGE, PROBE_EDGE, 0, 1};
    VkRect2D scissor = {{0, 0}, {PROBE_EDGE, PROBE_EDGE}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor};
    /* One blend state per attachment, blending disabled on both: the witness
     * measures the export path, not the blender. */
    VkPipelineColorBlendAttachmentState blend_state[2] = {
        {.colorWriteMask = 15}, {.colorWriteMask = 15}};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = blend_state};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = pipeline_layout, .renderPass = pass};
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                  &pipeline));

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0};
    TRY(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &command_allocate, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    if (!queue) goto cleanup;

    VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {PROBE_EDGE, PROBE_EDGE, 1}};
    VkImageMemoryBarrier staging_in = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE, .subresourceRange = staging_range};
    VkImageMemoryBarrier staging_out = staging_in;

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    TRY(vkBeginCommandBuffer(command, &begin));
    for (unsigned i = 0; i < 2; ++i) {
        staging_in.image = staging[i];
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &staging_in);
    }
    /* A clear both targets share and neither export contains: one clear value
     * per attachment, because Vulkan requires the array to cover every
     * attachment whose load operation is CLEAR. */
    VkClearValue clears[2] = {
        {.color = {.float32 = {0.125f, 0.25f, 0.5f, 0.25f}}},
        {.color = {.float32 = {0.125f, 0.25f, 0.5f, 0.25f}}}};
    VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer,
        .renderArea = {{0, 0}, {PROBE_EDGE, PROBE_EDGE}},
        .clearValueCount = 2, .pClearValues = clears};
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    /* Both targets are read back from the same draw. */
    for (unsigned i = 0; i < 2; ++i) {
        vkCmdCopyImage(command, image[i], VK_IMAGE_LAYOUT_GENERAL, staging[i],
                       VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        /* Publishing the copy to the host: the same two transitions the pinned
         * readback path records for its staging image. */
        staging_out.image = staging[i];
        staging_out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        staging_out.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        staging_out.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        staging_out.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &staging_out);
    }
    TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    TRY(vkQueueSubmit(queue, 1, &submit, fence));
    TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    TRY(vkResetFences(device, 1, &fence));
    for (unsigned i = 0; i < 2; ++i) {
        TRY(vkInvalidateMappedMemoryRanges(device, 1, &range[i]));
        VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout linear = {0};
        vkGetImageSubresourceLayout(device, staging[i], &subresource, &linear);
        if (!linear.rowPitch || linear.rowPitch < PROBE_EDGE * 4u ||
            linear.size > staging_requirements[i].size) goto cleanup;
        const VkDeviceSize pixel_offset = linear.offset +
            (VkDeviceSize)(PROBE_PIXEL / PROBE_EDGE) * linear.rowPitch +
            (VkDeviceSize)(PROBE_PIXEL % PROBE_EDGE) * 4u;
        memcpy(observed[i], staging_bytes[i] + pixel_offset, 4u);
    }

    /* The verdict is the same pure predicate the host regressions exercise, so
     * the artifact cannot report a decision the tests do not describe. */
    const int verified = ps5vk_two_mrt_verdict(observed[0], observed[1]);
    const int distinct = verified &&
        ((observed[0][0] > observed[1][0] + 2 * PS5VK_TWO_MRT_TOLERANCE ||
          observed[1][0] > observed[0][0] + 2 * PS5VK_TWO_MRT_TOLERANCE) ||
         (observed[0][1] > observed[1][1] + 2 * PS5VK_TWO_MRT_TOLERANCE ||
          observed[1][1] > observed[0][1] + 2 * PS5VK_TWO_MRT_TOLERANCE) ||
         (observed[0][2] > observed[1][2] + 2 * PS5VK_TWO_MRT_TOLERANCE ||
          observed[1][2] > observed[0][2] + 2 * PS5VK_TWO_MRT_TOLERANCE));
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_TWO_MRT_READBACK extent=%ux%u draws=1 pixel=%u "
        "target0=%02x%02x%02x%02x target1=%02x%02x%02x%02x "
        "expected_target0=%02x%02x%02x%02x expected_target1=%02x%02x%02x%02x "
        "tolerance=%u distinct=%d fence=success strict_verified=%d",
        PROBE_EDGE, PROBE_EDGE, (unsigned)PROBE_PIXEL,
        observed[0][0], observed[0][1], observed[0][2], observed[0][3],
        observed[1][0], observed[1][1], observed[1][2], observed[1][3],
        PS5VK_TWO_MRT_TARGET0_R, PS5VK_TWO_MRT_TARGET0_G,
        PS5VK_TWO_MRT_TARGET0_B, PS5VK_TWO_MRT_TARGET0_A,
        PS5VK_TWO_MRT_TARGET1_R, PS5VK_TWO_MRT_TARGET1_G,
        PS5VK_TWO_MRT_TARGET1_B, PS5VK_TWO_MRT_TARGET1_A,
        PS5VK_TWO_MRT_TOLERANCE, distinct, verified);
    if (!verified) goto cleanup;
    rc = VK_SUCCESS;

cleanup:
    if (device) (void)vkDeviceWaitIdle(device);
    for (unsigned i = 0; i < 2; ++i)
        if (staging_bytes[i]) vkUnmapMemory(device, staging_memory[i]);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (shaders[i]) vkDestroyShaderModule(device, shaders[i], NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    for (unsigned i = 0; i < 2; ++i) {
        if (view[i]) vkDestroyImageView(device, view[i], NULL);
        if (image[i]) vkDestroyImage(device, image[i], NULL);
        if (staging[i]) vkDestroyImage(device, staging[i], NULL);
        if (image_memory[i]) vkFreeMemory(device, image_memory[i], NULL);
        if (staging_memory[i]) vkFreeMemory(device, staging_memory[i], NULL);
    }
#undef TRY
    return rc;
}

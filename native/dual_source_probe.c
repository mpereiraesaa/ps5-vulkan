/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "dual_source_probe.h"
#include "dual_source_oracle.h"
#include "ps5log.h"
#include <string.h>

enum { PROBE_EDGE = 64, PROBE_BYTES = PROBE_EDGE * PROBE_EDGE * 4u };
/* The centre of the triangle the owned runtime vertex module draws. */
enum { PROBE_PIXEL = (PROBE_EDGE / 2) * PROBE_EDGE + PROBE_EDGE / 2 };
#define PROBE_GUARD UINT32_C(0x6d6d6d6d)

VkResult ps5vk_dual_source_probe(VkDevice device,
    const struct ps5vk_dual_source_probe_modules *modules)
{
    VkResult rc = VK_ERROR_UNKNOWN;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImage staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    unsigned char *staging_bytes = NULL;
    VkMemoryRequirements staging_requirements = {0};
    VkDeviceSize staging_allocation_bytes = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    unsigned char observed[2][4] = {{0}};

#define TRY(call) do { rc = (call); if (rc != VK_SUCCESS) { \
    ps5log_printf(PS5LOG_ERR, "PS5VK_DUAL_SOURCE_FAIL call=%s rc=%d", #call, rc); \
    goto cleanup; } } while (0)
    if (!device || !modules || !modules->vertex || !modules->vertex_words ||
        !modules->dual_fragment || !modules->dual_fragment_words)
        goto cleanup;

    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {PROBE_EDGE, PROBE_EDGE, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        /* The exact role the pinned linear readback accepts: a colour
         * attachment that is also a transfer source and destination. */
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    TRY(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements image_requirements;
    vkGetImageMemoryRequirements(device, image, &image_requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &allocation, NULL, &image_memory));
    TRY(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    TRY(vkCreateImageView(device, &view_info, NULL, &view));

    /* The readback is a transfer of the rendered target, and the attachment
     * plan this profile can serve leaves the target in GENERAL; the copy below
     * therefore reads it in that layout, exactly like the input-attachment
     * witness. */
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass};
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &view,
        .width = PROBE_EDGE, .height = PROBE_EDGE, .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    /* The pinned readback path is a copy from the tiled colour attachment into
     * a linear staging image, both in GENERAL. A buffer destination is not a
     * shape this profile serves, so the witness uses exactly the pair it does. */
    VkImageCreateInfo staging_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {PROBE_EDGE, PROBE_EDGE, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateImage(device, &staging_info, NULL, &staging));
    vkGetImageMemoryRequirements(device, staging, &staging_requirements);
    if (staging_requirements.size > UINT64_MAX - 4096u) goto cleanup;
    staging_allocation_bytes = staging_requirements.size + 4096u;
    allocation.allocationSize = staging_allocation_bytes;
    TRY(vkAllocateMemory(device, &allocation, NULL, &staging_memory));
    TRY(vkBindImageMemory(device, staging, staging_memory, 0));
    TRY(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0,
                    (void **)&staging_bytes));
    for (VkDeviceSize offset = 0; offset + 4 <= staging_allocation_bytes; offset += 4)
        memcpy(staging_bytes + offset, &(uint32_t){PROBE_GUARD}, 4);
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = staging_memory, .offset = 0, .size = staging_allocation_bytes};
    TRY(vkFlushMappedMemoryRanges(device, 1, &range));
    VkImageSubresourceRange staging_range = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout));
    const uint32_t *shader_words[2] = {modules->vertex, modules->dual_fragment};
    const size_t shader_counts[2] = {modules->vertex_words, modules->dual_fragment_words};
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
    /* Control: the same fragment module with blending disabled, so the target
     * receives the primary export alone. */
    VkPipelineColorBlendAttachmentState control_blend = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &control_blend};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = pipeline_layout, .renderPass = pass};
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                  &pipelines[0]));
    /* Candidate: the exact equation this profile accepts for SRC1. */
    VkPipelineColorBlendAttachmentState dual_blend = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 15};
    blend.pAttachments = &dual_blend;
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                  &pipelines[1]));

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
        .image = staging, .subresourceRange = staging_range};

    for (unsigned i = 0; i < 2; ++i) {
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(command, &begin));
        staging_in.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        staging_in.srcAccessMask = 0;
        staging_in.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &staging_in);
        /* A clear colour the blend never reads (dst factor is ZERO), so the
         * two reads can only differ through the blend state. */
        VkClearValue clear = {.color = {.float32 = {0.125f, 0.25f, 0.5f, 0.25f}}};
        VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass, .framebuffer = framebuffer,
            .renderArea = {{0, 0}, {PROBE_EDGE, PROBE_EDGE}},
            .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[i]);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        vkCmdCopyImage(command, image, VK_IMAGE_LAYOUT_GENERAL, staging,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        VkImageMemoryBarrier staging_out = staging_in;
        staging_out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        staging_out.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        staging_out.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        staging_out.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &staging_out);
        TRY(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
        TRY(vkResetFences(device, 1, &fence));
        TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
        VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout linear = {0};
        vkGetImageSubresourceLayout(device, staging, &subresource, &linear);
        if (!linear.rowPitch || linear.rowPitch < PROBE_EDGE * 4u ||
            linear.size > staging_requirements.size) goto cleanup;
        const VkDeviceSize pixel_offset = linear.offset +
            (VkDeviceSize)(PROBE_PIXEL / PROBE_EDGE) * linear.rowPitch +
            (VkDeviceSize)(PROBE_PIXEL % PROBE_EDGE) * 4u;
        memcpy(observed[i], staging_bytes + pixel_offset, 4u);
        /* Re-arm the canary for the second draw. */
        for (VkDeviceSize offset = 0; offset + 4 <= staging_allocation_bytes; offset += 4)
            memcpy(staging_bytes + offset, &(uint32_t){PROBE_GUARD}, 4);
    }

    /* The verdict is the same pure predicate the host regressions exercise, so
     * the artifact cannot report a decision the tests do not describe. */
    const int verified = ps5vk_dual_source_verdict(observed[0], observed[1]);
    const int distinct = verified &&
        (observed[0][1] > observed[1][1] + 2 * PS5VK_DUAL_SOURCE_TOLERANCE ||
         observed[1][1] > observed[0][1] + 2 * PS5VK_DUAL_SOURCE_TOLERANCE ||
         observed[0][2] > observed[1][2] + 2 * PS5VK_DUAL_SOURCE_TOLERANCE ||
         observed[1][2] > observed[0][2] + 2 * PS5VK_DUAL_SOURCE_TOLERANCE);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_DUAL_SOURCE_READBACK extent=%ux%u draws=2 pixel=%u "
        "control=%02x%02x%02x%02x candidate=%02x%02x%02x%02x "
        "expected_control=%02x%02x%02x%02x expected_candidate=%02x%02x%02x%02x "
        "tolerance=%u distinct=%d fence=success strict_verified=%d",
        PROBE_EDGE, PROBE_EDGE, (unsigned)PROBE_PIXEL,
        observed[0][0], observed[0][1], observed[0][2], observed[0][3],
        observed[1][0], observed[1][1], observed[1][2], observed[1][3],
        PS5VK_DUAL_SOURCE_CONTROL_R, PS5VK_DUAL_SOURCE_CONTROL_G,
        PS5VK_DUAL_SOURCE_CONTROL_B, PS5VK_DUAL_SOURCE_CONTROL_A,
        PS5VK_DUAL_SOURCE_BLEND_R, PS5VK_DUAL_SOURCE_BLEND_G,
        PS5VK_DUAL_SOURCE_BLEND_B, PS5VK_DUAL_SOURCE_BLEND_A,
        PS5VK_DUAL_SOURCE_TOLERANCE, distinct, verified);
    if (!verified) goto cleanup;
    rc = VK_SUCCESS;

cleanup:
    if (device) (void)vkDeviceWaitIdle(device);
    if (staging_bytes) vkUnmapMemory(device, staging_memory);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (pipelines[i]) vkDestroyPipeline(device, pipelines[i], NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (shaders[i]) vkDestroyShaderModule(device, shaders[i], NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    if (view) vkDestroyImageView(device, view, NULL);
    if (image) vkDestroyImage(device, image, NULL);
    if (staging) vkDestroyImage(device, staging, NULL);
    if (image_memory) vkFreeMemory(device, image_memory, NULL);
    if (staging_memory) vkFreeMemory(device, staging_memory, NULL);
#undef TRY
    return rc;
}

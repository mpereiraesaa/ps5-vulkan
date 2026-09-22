/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sample_rate_probe.h"
#include "color_clear.h"
#include "sample_rate_contract.h"
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
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline pipeline = VK_NULL_HANDLE;
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
    if (rc == VK_SUCCESS) {
        /* Phase two: the same target, drawn through a pipeline that asks for
         * per-sample shading. The module writes gl_SampleID, so a draw the
         * hardware iterates per sample leaves one distinct value per sample
         * plane, while a once-per-pixel draw can only ever leave one value in
         * the whole surface. The oracle is that difference - not the values
         * themselves, which depend on how the hardware numbers its samples. */
        VkPipelineLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        PROBE_TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
        for (unsigned i = 0; i < 2; ++i) {
            VkShaderModuleCreateInfo module_info = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = (i ? params->fragment_words : params->vertex_words) * 4u,
                .pCode = i ? params->fragment : params->vertex};
            PROBE_TRY(vkCreateShaderModule(device, &module_info, NULL, &modules[i]));
        }
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vertex_input = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineRasterizationStateCreateInfo raster = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
        VkSampleMask full_mask = ps5vk_sample_count_full_mask(params->samples);
        VkPipelineMultisampleStateCreateInfo multisample = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = params->samples,
            .sampleShadingEnable = VK_TRUE, .minSampleShading = 1.0f,
            .pSampleMask = &full_mask};
        VkViewport viewport = {0.0f, 0.0f, (float)params->extent, (float)params->extent, 0.0f, 1.0f};
        VkRect2D scissor = {{0, 0}, {params->extent, params->extent}};
        VkPipelineViewportStateCreateInfo viewport_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .pViewports = &viewport,
            .scissorCount = 1, .pScissors = &scissor};
        VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xfu};
        VkPipelineColorBlendStateCreateInfo blend = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &blend_attachment};
        VkGraphicsPipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .layout = layout, .renderPass = pass, .subpass = 0, .stageCount = 2,
            .pStages = stages, .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &assembly, .pRasterizationState = &raster,
            .pMultisampleState = &multisample, .pViewportState = &viewport_state,
            .pColorBlendState = &blend};
        PROBE_TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
            NULL, &pipeline));
        PROBE_TRY(vkBeginCommandBuffer(command, &begin));
        vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        PROBE_TRY(vkEndCommandBuffer(command));
        PROBE_TRY(vkQueueSubmit(queue, 1, &submit, fence));
        PROBE_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
        PROBE_TRY(vkResetFences(device, 1, &fence));
        PROBE_TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
        /* The shaded values are counted apart from the clear word: a
         * multisampled surface's tiling leaves padding the clear filled and the
         * draw never touches, so the clear is expected to survive somewhere and
         * is not a shaded value. What the oracle requires is that the draw left
         * exactly one distinct value per sample, each of them the value the
         * module writes for that sample index (R = sampleID/255, alpha 1),
         * which a once-per-pixel draw cannot produce at all. */
        const unsigned expected_distinct = ps5vk_sample_count_number(params->samples);
        unsigned shaded = 0, shaded_seen[PROBE_DISTINCT_LIMIT] = {0};
        correct = 0; first = 0; last = 0;
        unsigned covered = 0, matched = 0;
        for (unsigned i = 0; i < words; ++i) {
            uint32_t word;
            memcpy(&word, (const unsigned char *)mapped + (size_t)i * 4u, sizeof(word));
            if (!i) first = word;
            last = word;
            if (word == expected) continue;
            ++covered;
            unsigned found = 0;
            for (unsigned k = 0; k < shaded && !found; ++k) found = shaded_seen[k] == word;
            if (!found && shaded < PROBE_DISTINCT_LIMIT) shaded_seen[shaded++] = word;
        }
        for (unsigned sample = 0; sample < expected_distinct; ++sample) {
            const uint32_t want = UINT32_C(0xff000000) | ((uint32_t)sample << 16u);
            for (unsigned k = 0; k < shaded; ++k)
                if (shaded_seen[k] == want) { ++matched; break; }
        }
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_SAMPLE_RATE_SHADED extent=%ux%u samples=%u words=%u shaded_values=%u "
            "expected_values=%u matched=%u covered_words=%u values=%08x,%08x,%08x,%08x "
            "verdict=%u",
            params->extent, params->extent, (unsigned)params->samples, words, shaded,
            expected_distinct, matched, covered,
            shaded_seen[0], shaded_seen[1], shaded_seen[2], shaded_seen[3],
            (unsigned)(shaded == expected_distinct && matched == expected_distinct));
        rc = (shaded == expected_distinct && matched == expected_distinct) ?
            VK_SUCCESS : VK_ERROR_UNKNOWN;
    }

cleanup:
    if (mapped) vkUnmapMemory(device, memory);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (modules[i]) vkDestroyShaderModule(device, modules[i], NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
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

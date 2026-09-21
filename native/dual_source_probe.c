/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "dual_source_probe.h"
#include "ps5log.h"
#include <string.h>

enum { PROBE_EDGE = 64, PROBE_BYTES = PROBE_EDGE * PROBE_EDGE * 4u };
/* The centre of the triangle the owned runtime vertex module draws. */
enum { PROBE_PIXEL = (PROBE_EDGE / 2) * PROBE_EDGE + PROBE_EDGE / 2 };

/* experiments/graphics/runtime_dual_source.frag exports the two sources of
 * attachment zero: primary (0.25,0.50,0.75,1.0) at Location 0 Index 0 and
 * secondary (0.80,0.40,0.20,0.50) at Location 0 Index 1.  With blending
 * disabled the target receives the primary.  With
 * color = SRC1_COLOR x src + ZERO x dst and alpha = ONE x src + ZERO x dst it
 * receives primary.rgb * secondary.rgb with the primary alpha, and neither read
 * depends on the clear colour, so the two draws differ only in blend state.
 * The products are exactly representable in UNORM8 (0.20 -> 51, 0.15 -> 38);
 * the primary channels are not (0.25/0.50/0.75 -> 63.75/127.5/191.25), so the
 * control is judged inside one LSB and the candidate on its exact bytes. */
enum { PROBE_PRIMARY_R = 64, PROBE_PRIMARY_G = 128, PROBE_PRIMARY_B = 191,
       PROBE_BLEND_R = 51, PROBE_BLEND_G = 51, PROBE_BLEND_B = 38,
       PROBE_TOLERANCE = 1 };

static int within(unsigned observed, unsigned expected)
{
    return observed + PROBE_TOLERANCE >= expected &&
        observed <= expected + PROBE_TOLERANCE;
}

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
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    uint8_t *mapped = NULL;
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
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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

    /* The readback is a transfer of the rendered target, so the pass leaves it
     * in the layout that copy reads. */
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
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

    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = PROBE_BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    TRY(vkCreateBuffer(device, &buffer_info, NULL, &staging));
    VkMemoryRequirements buffer_requirements;
    vkGetBufferMemoryRequirements(device, staging, &buffer_requirements);
    allocation.allocationSize = buffer_requirements.size;
    TRY(vkAllocateMemory(device, &allocation, NULL, &staging_memory));
    TRY(vkBindBufferMemory(device, staging, staging_memory, 0));
    TRY(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = staging_memory, .offset = 0, .size = VK_WHOLE_SIZE};

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
    VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {PROBE_EDGE, PROBE_EDGE, 1}};

    for (unsigned i = 0; i < 2; ++i) {
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(command, &begin));
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
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging, 1, &region);
        TRY(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
        TRY(vkResetFences(device, 1, &fence));
        TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
        for (unsigned channel = 0; channel < 4; ++channel)
            observed[i][channel] = mapped[PROBE_PIXEL * 4u + channel];
    }

    const int control_ok = within(observed[0][0], PROBE_PRIMARY_R) &&
        within(observed[0][1], PROBE_PRIMARY_G) &&
        within(observed[0][2], PROBE_PRIMARY_B) && observed[0][3] == 255;
    const int candidate_ok = within(observed[1][0], PROBE_BLEND_R) &&
        within(observed[1][1], PROBE_BLEND_G) &&
        within(observed[1][2], PROBE_BLEND_B) && observed[1][3] == 255;
    /* The delta is the actual claim: the blender consumed the secondary export
     * of the fragment module instead of rendering the primary twice. */
    const int distinct = observed[0][1] > observed[1][1] + 2 * PROBE_TOLERANCE ||
        observed[1][1] > observed[0][1] + 2 * PROBE_TOLERANCE ||
        observed[0][2] > observed[1][2] + 2 * PROBE_TOLERANCE ||
        observed[1][2] > observed[0][2] + 2 * PROBE_TOLERANCE;
    const int verified = control_ok && candidate_ok && distinct;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_DUAL_SOURCE_READBACK extent=%ux%u draws=2 pixel=%u "
        "control=%02x%02x%02x%02x candidate=%02x%02x%02x%02x "
        "expected_control=%02x%02x%02x%02x expected_candidate=%02x%02x%02x%02x "
        "tolerance=%u distinct=%d fence=success strict_verified=%d",
        PROBE_EDGE, PROBE_EDGE, (unsigned)PROBE_PIXEL,
        observed[0][0], observed[0][1], observed[0][2], observed[0][3],
        observed[1][0], observed[1][1], observed[1][2], observed[1][3],
        PROBE_PRIMARY_R, PROBE_PRIMARY_G, PROBE_PRIMARY_B, 255,
        PROBE_BLEND_R, PROBE_BLEND_G, PROBE_BLEND_B, 255,
        PROBE_TOLERANCE, distinct, verified);
    if (!verified) goto cleanup;
    rc = VK_SUCCESS;

cleanup:
    if (device) (void)vkDeviceWaitIdle(device);
    if (mapped) vkUnmapMemory(device, staging_memory);
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
    if (staging) vkDestroyBuffer(device, staging, NULL);
    if (image_memory) vkFreeMemory(device, image_memory, NULL);
    if (staging_memory) vkFreeMemory(device, staging_memory, NULL);
#undef TRY
    return rc;
}

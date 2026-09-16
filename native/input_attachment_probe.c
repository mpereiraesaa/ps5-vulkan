/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "input_attachment_probe.h"
#include "input_attachment_oracle.h"
#include "input_attachment_gate.h"
#include "graphics_pipeline_ps5.h"
#include "vk_pipeline.h"
#include "descriptor_table_layout.h"
#include "ps5log.h"
#include <stdlib.h>
#include <string.h>

enum { PROBE_EDGE = 64, PROBE_PIXELS = PROBE_EDGE * PROBE_EDGE };
#define PROBE_GUARD UINT32_C(0x6b6b6b6b)

static uint32_t fnv1a(const void *data, size_t bytes)
{
    const unsigned char *p = data;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    return hash;
}

static const char *verdict_name(enum ps5vk_input_attachment_oracle_verdict verdict)
{
    switch (verdict) {
    case PS5VK_INPUT_ATTACHMENT_ORACLE_OK: return "ok";
    case PS5VK_INPUT_ATTACHMENT_ORACLE_SKIPPED: return "skipped";
    case PS5VK_INPUT_ATTACHMENT_ORACLE_PASS_THROUGH: return "pass-through";
    case PS5VK_INPUT_ATTACHMENT_ORACLE_CONSTANT: return "constant";
    default: return "mismatch";
    }
}

VkResult ps5vk_input_attachment_probe(VkDevice device,
    const struct ps5vk_input_attachment_probe_modules *modules)
{
    VkResult rc = VK_ERROR_UNKNOWN;
    VkImage target = VK_NULL_HANDLE, staging = VK_NULL_HANDLE;
    VkDeviceMemory target_memory = VK_NULL_HANDLE, staging_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shaders[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    unsigned char *staging_bytes = NULL;
    VkMemoryRequirements target_requirements = {0}, staging_requirements = {0};
    VkDeviceSize staging_allocation_bytes = 0;

#define TRY(call) do { rc = (call); if (rc != VK_SUCCESS) { \
    ps5log_printf(PS5LOG_ERR, "PS5VK_INPUT_ATTACHMENT_FAIL call=%s rc=%d", #call, rc); \
    goto cleanup; } } while (0)
    if (!device || !modules || !modules->vertex || !modules->vertex_words ||
        !modules->pattern_fragment || !modules->pattern_fragment_words ||
        !modules->transform_fragment || !modules->transform_fragment_words)
        goto cleanup;

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageCreateInfo target_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {PROBE_EDGE, PROBE_EDGE, 1}, .mipLevels = 1,
        .arrayLayers = PS5VK_INPUT_ATTACHMENT_LAYER_COUNT,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    TRY(vkCreateImage(device, &target_info, NULL, &target));
    vkGetImageMemoryRequirements(device, target, &target_requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = target_requirements.size, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &allocation, NULL, &target_memory));
    TRY(vkBindImageMemory(device, target, target_memory, 0));
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_INPUT_ATTACHMENT_RESOURCE create=success bind=success "
        "allocation_bytes=%llu layers=%u usage=%u",
        (unsigned long long)target_requirements.size,
        PS5VK_INPUT_ATTACHMENT_LAYER_COUNT,usage);

    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = target, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    TRY(vkCreateImageView(device, &view_info, NULL, &view));

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference input = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpasses[2] = {
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color},
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .inputAttachmentCount = 1, .pInputAttachments = &input,
         .colorAttachmentCount = 1, .pColorAttachments = &color}};
    VkSubpassDependency dependency = {
        .srcSubpass = 0, .dstSubpass = 1,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 2, .pSubpasses = subpasses,
        .dependencyCount = 1, .pDependencies = &dependency};
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view,
        .width = PROBE_EDGE, .height = PROBE_EDGE, .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    TRY(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    TRY(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo set_allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1,
        .pSetLayouts = &set_layout};
    TRY(vkAllocateDescriptorSets(device, &set_allocate, &descriptor_set));
    VkDescriptorImageInfo descriptor_image = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
        .pImageInfo = &descriptor_image};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    TRY(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout));

    const uint32_t *shader_words[3] = {modules->vertex, modules->pattern_fragment,
        modules->transform_fragment};
    const size_t shader_counts[3] = {modules->vertex_words,
        modules->pattern_fragment_words, modules->transform_fragment_words};
    for (unsigned i = 0; i < 3; ++i) {
        VkShaderModuleCreateInfo shader_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shader_counts[i] * 4u, .pCode = shader_words[i]};
        TRY(vkCreateShaderModule(device, &shader_info, NULL, &shaders[i]));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shaders[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
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
    VkPipelineColorBlendAttachmentState color_blend = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &color_blend};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = pipeline_layout, .renderPass = pass};
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                  &pipelines[0]));
    stages[1].module = shaders[2];
    pipeline_info.subpass = 1;
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                  &pipelines[1]));

    const struct ps5vk_native_graphics_pipeline *native = pipelines[1]->graphics_state;
    if (!native || !native->pair || !native->pair->ready) goto cleanup;
    const struct ps5vk_runtime_draw_abi *abi = &native->pair->runtime_arguments;
    if (!abi->enabled || !abi->fragment_descriptor_valid[0] ||
        abi->vertex_descriptor_valid[0] ||
        !(abi->fragment_used_bindings[0] & UINT64_C(1)) ||
        abi->fragment_descriptor_slot[0] >= abi->fragment_count ||
        ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) != 32)
        goto cleanup;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_INPUT_ATTACHMENT_ABI set=0 binding=0 record_bytes=32 fragment_slot=%u "
        "fragment_count=%u used_bindings=%016llx vertex_table=0",
        abi->fragment_descriptor_slot[0], abi->fragment_count,
        (unsigned long long)abi->fragment_used_bindings[0]);

    VkImageCreateInfo staging_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {PROBE_EDGE, PROBE_EDGE, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateImage(device, &staging_info, NULL, &staging));
    vkGetImageMemoryRequirements(device, staging, &staging_requirements);
    /* Bind a larger allocation while preserving the image's exact 64x64
     * contract. The extra page is an unconditional canary region even on a
     * backend whose row pitch happens to be tightly packed. */
    if (staging_requirements.size > UINT64_MAX - 4096u) goto cleanup;
    staging_allocation_bytes = staging_requirements.size + 4096u;
    allocation.allocationSize = staging_allocation_bytes;
    TRY(vkAllocateMemory(device, &allocation, NULL, &staging_memory));
    TRY(vkBindImageMemory(device, staging, staging_memory, 0));
    TRY(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0,
                    (void **)&staging_bytes));
    for (VkDeviceSize offset = 0; offset + 4 <= staging_allocation_bytes; offset += 4)
        memcpy(staging_bytes + offset, &(uint32_t){PROBE_GUARD}, 4);
    VkMappedMemoryRange mapped = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = staging_memory, .offset = 0, .size = staging_allocation_bytes};
    TRY(vkFlushMappedMemoryRanges(device, 1, &mapped));

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

    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    TRY(vkBeginCommandBuffer(command, &begin));
    VkClearValue clear = {.color = {.float32 = {16.0f/255.0f, 16.0f/255.0f,
                                                16.0f/255.0f, 1.0f}}};
    VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer,
        .renderArea = {{0, 0}, {PROBE_EDGE, PROBE_EDGE}},
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[1]);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    TRY(vkQueueSubmit(queue, 1, &submit, fence));
    TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    ps5log_line(PS5LOG_MARK,
        "PS5VK_INPUT_ATTACHMENT_DRAW subpasses=2 draws=2 boundary=required "
        "dependency=colour-write-to-fragment-input-read-by-region");

    TRY(vkResetCommandBuffer(command, 0));
    TRY(vkResetFences(device, 1, &fence));
    TRY(vkBeginCommandBuffer(command, &begin));
    VkImageSubresourceRange staging_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier staging_in = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = staging, .subresourceRange = staging_range};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &staging_in);
    VkImageCopy copy = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {PROBE_EDGE, PROBE_EDGE, 1}};
    vkCmdCopyImage(command, target, VK_IMAGE_LAYOUT_GENERAL, staging,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    VkImageMemoryBarrier staging_out = staging_in;
    staging_out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    staging_out.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    staging_out.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &staging_out);
    TRY(vkEndCommandBuffer(command));
    TRY(vkQueueSubmit(queue, 1, &submit, fence));
    TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    TRY(vkInvalidateMappedMemoryRanges(device, 1, &mapped));
    ps5log_line(PS5LOG_MARK,
        "PS5VK_INPUT_ATTACHMENT_COPY_COMPLETED source=tiled-color "
        "destination=linear-staging extent=64x64 layers=1");

    VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout linear = {0};
    vkGetImageSubresourceLayout(device, staging, &subresource, &linear);
    if (!linear.rowPitch || linear.size > staging_requirements.size ||
        linear.rowPitch < PROBE_EDGE * 4u) goto cleanup;
    uint32_t pixels[PROBE_PIXELS];
    unsigned long guard_words = 0, guard_mismatches = 0;
    for (uint32_t y = 0; y < PROBE_EDGE; ++y) {
        memcpy(&pixels[y * PROBE_EDGE], staging_bytes + linear.offset +
               (VkDeviceSize)y * linear.rowPitch, PROBE_EDGE * 4u);
        for (VkDeviceSize x = PROBE_EDGE * 4u; x + 4 <= linear.rowPitch; x += 4) {
            uint32_t word = 0;
            memcpy(&word, staging_bytes + linear.offset +
                   (VkDeviceSize)y * linear.rowPitch + x, 4);
            ++guard_words;
            guard_mismatches += word != PROBE_GUARD;
        }
    }
    for (VkDeviceSize offset = staging_requirements.size;
         offset + 4 <= staging_allocation_bytes; offset += 4) {
        uint32_t word = 0;
        memcpy(&word, staging_bytes + offset, 4);
        ++guard_words;
        guard_mismatches += word != PROBE_GUARD;
    }
    struct ps5vk_input_attachment_oracle_result result =
        ps5vk_input_attachment_oracle_verify(pixels, PROBE_EDGE, PROBE_EDGE);
    uint32_t expected[PROBE_PIXELS];
    for (uint32_t y = 0; y < PROBE_EDGE; ++y)
        for (uint32_t x = 0; x < PROBE_EDGE; ++x)
            expected[y * PROBE_EDGE + x] = ps5vk_input_attachment_oracle_expected(x, y);
    const uint32_t actual_hash = fnv1a(pixels, sizeof(pixels));
    const uint32_t expected_hash = fnv1a(expected, sizeof(expected));
    const int verified = result.verdict == PS5VK_INPUT_ATTACHMENT_ORACLE_OK &&
        result.matched == result.total && !guard_mismatches &&
        actual_hash == expected_hash;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_INPUT_ATTACHMENT_READBACK extent=%ux%u layers=6 view_layer=0 "
        "matched=%lu total=%lu verdict=%s first_x=%u first_y=%u "
        "first_word=%08x expected_first=%08x "
        "actual_hash=%08x expected_hash=%08x guard_words=%lu guard_mismatches=%lu "
        "strict_verified=%d",
        PROBE_EDGE, PROBE_EDGE, result.matched, result.total,
        verdict_name(result.verdict), result.first_x, result.first_y,
        pixels[0], expected[0],
        actual_hash, expected_hash, guard_words, guard_mismatches, verified);
    if (!verified) goto cleanup;
    rc = VK_SUCCESS;

cleanup:
    if (device) (void)vkDeviceWaitIdle(device);
    if (staging_bytes) vkUnmapMemory(device, staging_memory);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    for (unsigned i = 0; i < 2; ++i) if (pipelines[i]) vkDestroyPipeline(device, pipelines[i], NULL);
    for (unsigned i = 0; i < 3; ++i) if (shaders[i]) vkDestroyShaderModule(device, shaders[i], NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    if (view) vkDestroyImageView(device, view, NULL);
    if (staging) vkDestroyImage(device, staging, NULL);
    if (target) vkDestroyImage(device, target, NULL);
    if (staging_memory) vkFreeMemory(device, staging_memory, NULL);
    if (target_memory) vkFreeMemory(device, target_memory, NULL);
#undef TRY
    return rc;
}

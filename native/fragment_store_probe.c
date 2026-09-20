/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "fragment_store_probe.h"
#include "graphics_pipeline_ps5.h"
#include "vk_pipeline.h"
#include "descriptor_table_layout.h"
#include "ps5log.h"
#include <string.h>

enum { PROBE_EDGE = 64, PROBE_PIXELS = PROBE_EDGE * PROBE_EDGE,
       PROBE_WORDS = 16, PROBE_BYTES = PROBE_WORDS * 4 };
#define PROBE_GUARD UINT32_C(0x6d6d6d6d)

VkResult ps5vk_fragment_store_probe(VkDevice device,
    const struct ps5vk_fragment_store_probe_modules *modules)
{
    VkResult rc = VK_ERROR_UNKNOWN;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t *mapped[2] = {NULL, NULL};
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shaders[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

#define TRY(call) do { rc = (call); if (rc != VK_SUCCESS) { \
    ps5log_printf(PS5LOG_ERR, "PS5VK_FRAGMENT_STORE_FAIL call=%s rc=%d", #call, rc); \
    goto cleanup; } } while (0)
    if (!device || !modules || !modules->vertex || !modules->vertex_words ||
        !modules->control_fragment || !modules->control_fragment_words ||
        !modules->atomic_fragment || !modules->atomic_fragment_words)
        goto cleanup;

    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {PROBE_EDGE, PROBE_EDGE, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
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

    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
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
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &view,
        .width = PROBE_EDGE, .height = PROBE_EDGE, .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    TRY(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    TRY(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetLayout layouts[2] = {set_layout, set_layout};
    VkDescriptorSetAllocateInfo set_allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 2,
        .pSetLayouts = layouts};
    TRY(vkAllocateDescriptorSets(device, &set_allocate, sets));

    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = PROBE_BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkDescriptorBufferInfo descriptor_buffers[2];
    VkWriteDescriptorSet writes[2];
    VkMappedMemoryRange ranges[2];
    for (unsigned i = 0; i < 2; ++i) {
        TRY(vkCreateBuffer(device, &buffer_info, NULL, &buffers[i]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        allocation.allocationSize = requirements.size;
        TRY(vkAllocateMemory(device, &allocation, NULL, &memories[i]));
        TRY(vkBindBufferMemory(device, buffers[i], memories[i], 0));
        TRY(vkMapMemory(device, memories[i], 0, VK_WHOLE_SIZE, 0,
                        (void **)&mapped[i]));
        mapped[i][0] = 0;
        for (unsigned word = 1; word < PROBE_WORDS; ++word)
            mapped[i][word] = PROBE_GUARD;
        ranges[i] = (VkMappedMemoryRange){.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[i], .offset = 0, .size = VK_WHOLE_SIZE};
        descriptor_buffers[i] = (VkDescriptorBufferInfo){.buffer = buffers[i],
            .offset = 0, .range = PROBE_BYTES};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = sets[i], .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &descriptor_buffers[i]};
    }
    TRY(vkFlushMappedMemoryRanges(device, 2, ranges));
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout));
    const uint32_t *shader_words[3] = {modules->vertex,
        modules->control_fragment, modules->atomic_fragment};
    const size_t shader_counts[3] = {modules->vertex_words,
        modules->control_fragment_words, modules->atomic_fragment_words};
    for (unsigned i = 0; i < 3; ++i) {
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
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                  &pipelines[1]));

    const struct ps5vk_native_graphics_pipeline *control = pipelines[0]->graphics_state;
    const struct ps5vk_native_graphics_pipeline *candidate = pipelines[1]->graphics_state;
    if (!control || !candidate || !control->pair || !candidate->pair ||
        control->pair->runtime_arguments.fragment_descriptor_valid[0] ||
        !candidate->pair->runtime_arguments.fragment_descriptor_valid[0] ||
        candidate->pair->runtime_arguments.vertex_descriptor_valid[0] ||
        !(candidate->pair->runtime_arguments.fragment_used_bindings[0] & UINT64_C(1)) ||
        ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) != 16)
        goto cleanup;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_FRAGMENT_STORE_ABI set=0 binding=0 record_bytes=16 "
        "control_table=0 candidate_fragment_table=1 used_bindings=%016llx",
        (unsigned long long)candidate->pair->runtime_arguments.fragment_used_bindings[0]);

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
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    TRY(vkBeginCommandBuffer(command, &begin));
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 1}}};
    VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer,
        .renderArea = {{0, 0}, {PROBE_EDGE, PROBE_EDGE}},
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    for (unsigned i = 0; i < 2; ++i) {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_layout, 0, 1, &sets[i], 0, NULL);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[i]);
        vkCmdDraw(command, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(command);
    TRY(vkEndCommandBuffer(command));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    TRY(vkQueueSubmit(queue, 1, &submit, fence));
    TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    TRY(vkInvalidateMappedMemoryRanges(device, 2, ranges));

    unsigned guard_mismatches = 0;
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned word = 1; word < PROBE_WORDS; ++word)
            guard_mismatches += mapped[i][word] != PROBE_GUARD;
    const int verified = mapped[0][0] == 0 && mapped[1][0] == PROBE_PIXELS &&
        !guard_mismatches;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_FRAGMENT_STORE_READBACK extent=%ux%u draws=2 expected=%u "
        "control_counter=%u candidate_counter=%u guard_words=%u "
        "guard_mismatches=%u fence=success strict_verified=%d",
        PROBE_EDGE, PROBE_EDGE, PROBE_PIXELS, mapped[0][0], mapped[1][0],
        2u * (PROBE_WORDS - 1u), guard_mismatches, verified);
    if (!verified) goto cleanup;
    rc = VK_SUCCESS;

cleanup:
    if (device) (void)vkDeviceWaitIdle(device);
    for (unsigned i = 0; i < 2; ++i)
        if (mapped[i]) vkUnmapMemory(device, memories[i]);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (pipelines[i]) vkDestroyPipeline(device, pipelines[i], NULL);
    for (unsigned i = 0; i < 3; ++i)
        if (shaders[i]) vkDestroyShaderModule(device, shaders[i], NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    if (view) vkDestroyImageView(device, view, NULL);
    if (image) vkDestroyImage(device, image, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (buffers[i]) vkDestroyBuffer(device, buffers[i], NULL);
    if (image_memory) vkFreeMemory(device, image_memory, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (memories[i]) vkFreeMemory(device, memories[i], NULL);
#undef TRY
    return rc;
}

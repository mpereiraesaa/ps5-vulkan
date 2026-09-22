/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sample_rate_probe.h"
#include "color_clear.h"
#include "sample_rate_contract.h"
#include "vk_image.h"
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

/* The shape walk's own step log. Each step is announced BEFORE the call runs,
 * so a step that never returns is named by the log it left behind, and a
 * refusal names both the step and the Vulkan result. */
static int shape_step(const char *name, VkResult rc)
{
    ps5log_printf(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=%s rc=%d ok=%u",
        name, (int)rc, (unsigned)(rc == VK_SUCCESS));
    return rc == VK_SUCCESS;
}

VkResult ps5vk_sample_rate_shape_probe(VkDevice device,
    const struct ps5vk_sample_rate_shape_params *params)
{
    VkImage images[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView views[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule modules[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline pipelines[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory ubo_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *ubo_mapped = NULL;
    VkResult rc = VK_SUCCESS;
    const uint32_t count = ps5vk_sample_count_number(params->samples);
    if (!device || !params || !params->extent || !count || count < 2u ||
        !params->vertex || !params->vertex_words ||
        !params->write_fragment || !params->write_fragment_words ||
        !params->fetch_fragment || !params->fetch_fragment_words ||
        !params->sample_fragment || !params->sample_fragment_words ||
        !params->fetch_const_fragment || !params->fetch_const_fragment_words ||
        !params->resolve_fragment || !params->resolve_fragment_words)
        return VK_ERROR_INITIALIZATION_FAILED;

#define SHAPE_TRY(call) do { rc = (call); if (!shape_step(#call, rc)) goto cleanup; } while (0)
    /* Step 1: the multisampled colour attachment with the exact usage the
     * pinned oracle builds, including the role only the fetch pass needs. The
     * format is the oracle's own - R8G8B8A8_UNORM - because the multi-role
     * colour shapes (the attachment with its readback pair) exist for the one
     * colour row that carries the readback capability; a first walk with BGRA8
     * stopped at its own format choice, which is what named that. */
    {
        const VkImageCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {params->extent, params->extent, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = params->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        SHAPE_TRY(vkCreateImage(device, &info, NULL, &images[0]));
    }
    /* Steps 2 and 3: the resolve target and the two per-sample targets, each
     * single sample with the readback pair the oracle gives them. */
    for (unsigned i = 1; i < 4; ++i) {
        const VkImageCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {params->extent, params->extent, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        ps5log_printf(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=create_single_target index=%u", i);
        rc = vkCreateImage(device, &info, NULL, &images[i]);
        if (!shape_step("create_single_target", rc)) goto cleanup;
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, images[i], &requirements);
        const VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = 0};
        SHAPE_TRY(vkAllocateMemory(device, &allocation, NULL, &memories[i]));
        SHAPE_TRY(vkBindImageMemory(device, images[i], memories[i], 0));
        const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        SHAPE_TRY(vkCreateImageView(device, &view_info, NULL, &views[i]));
    }
    /* The multisampled attachment is bound and viewed like any other. */
    {
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, images[0], &requirements);
        const VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = 0};
        SHAPE_TRY(vkAllocateMemory(device, &allocation, NULL, &memories[0]));
        SHAPE_TRY(vkBindImageMemory(device, images[0], memories[0], 0));
        const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[0],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        SHAPE_TRY(vkCreateImageView(device, &view_info, NULL, &views[0]));
    }
    /* Step 4: the descriptor set the fetch stage reads: its input attachment
     * plus the uniform sample index upstream passes the same way. */
    {
        const VkDescriptorSetLayoutBinding bindings[2] = {
            {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
        const VkDescriptorSetLayoutCreateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = bindings};
        SHAPE_TRY(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
        const VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        SHAPE_TRY(vkCreateBuffer(device, &buffer_info, NULL, &ubo));
        VkMemoryRequirements buffer_requirements;
        vkGetBufferMemoryRequirements(device, ubo, &buffer_requirements);
        const VkMemoryAllocateInfo buffer_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = buffer_requirements.size, .memoryTypeIndex = 0};
        SHAPE_TRY(vkAllocateMemory(device, &buffer_allocation, NULL, &ubo_memory));
        SHAPE_TRY(vkBindBufferMemory(device, ubo, ubo_memory, 0));
        /* The mapping covers the whole allocation: a partial mapping cannot
         * be flushed, because the range rule requires the flush to cover a
         * whole number of non-coherent atoms or end at the allocation. */
        SHAPE_TRY(vkMapMemory(device, ubo_memory, 0, VK_WHOLE_SIZE, 0, &ubo_mapped));
        memset(ubo_mapped, 0, 4);
        const VkDescriptorPoolSize sizes[2] = {
            {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
        const VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
        SHAPE_TRY(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
        const VkDescriptorSetAllocateInfo set_allocate = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
        SHAPE_TRY(vkAllocateDescriptorSets(device, &set_allocate, &set));
        const VkDescriptorImageInfo input_info = {.imageView = views[0],
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo buffer_binding = {.buffer = ubo, .offset = 0, .range = 4};
        const VkWriteDescriptorSet writes[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
             .pImageInfo = &input_info},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .pBufferInfo = &buffer_binding}};
        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    }
    /* Step 5: the pass the oracle builds - subpass 0 renders the multisampled
     * attachment and resolves it, subpasses 1 and 2 read it once per sample and
     * preserve the target their sibling renders into. */
    {
        const VkAttachmentDescription attachments[4] = {
            {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = params->samples,
             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
             .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
             .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
             .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
             .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        const VkAttachmentReference color0 = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference resolve0 = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference input0 = {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkAttachmentReference color1 = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference color2 = {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const uint32_t preserve1[1] = {3};
        const uint32_t preserve2[1] = {2};
        const VkSubpassDescription subpasses[3] = {
            {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
             .colorAttachmentCount = 1, .pColorAttachments = &color0,
             .pResolveAttachments = &resolve0},
            {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
             .inputAttachmentCount = 1, .pInputAttachments = &input0,
             .colorAttachmentCount = 1, .pColorAttachments = &color1,
             .preserveAttachmentCount = 1, .pPreserveAttachments = preserve1},
            {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
             .inputAttachmentCount = 1, .pInputAttachments = &input0,
             .colorAttachmentCount = 1, .pColorAttachments = &color2,
             .preserveAttachmentCount = 1, .pPreserveAttachments = preserve2}};
        const VkRenderPassCreateInfo pass_info = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 4, .pAttachments = attachments,
            .subpassCount = 3, .pSubpasses = subpasses};
        SHAPE_TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
        const VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
            .attachmentCount = 4, .pAttachments = views,
            .width = params->extent, .height = params->extent, .layers = 1};
        SHAPE_TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));
    }
    /* Steps 6 to 8: the pipelines. Subpass 0 is the multisampled draw with no
     * input attachment; subpass 1 is the per-sample fetch stage, which is the
     * one whose module reads the multisampled input attachment. */
    {
        const VkPipelineLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &set_layout};
        SHAPE_TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
        const uint32_t *sources[3] = {params->vertex, params->write_fragment, params->fetch_fragment};
        const size_t words[3] = {params->vertex_words, params->write_fragment_words,
                                 params->fetch_fragment_words};
        for (unsigned i = 0; i < 3; ++i) {
            const VkShaderModuleCreateInfo module_info = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = words[i] * 4u, .pCode = sources[i]};
            SHAPE_TRY(vkCreateShaderModule(device, &module_info, NULL, &modules[i]));
        }
        const VkPipelineShaderStageCreateInfo stages[3][2] = {
            {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", NULL},
             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_FRAGMENT_BIT, modules[1], "main", NULL}},
            {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", NULL},
             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_FRAGMENT_BIT, modules[2], "main", NULL}},
        };
        const VkPipelineVertexInputStateCreateInfo vertex_input = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineInputAssemblyStateCreateInfo assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        const VkPipelineRasterizationStateCreateInfo raster = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
        const VkViewport viewport = {0.0f, 0.0f, (float)params->extent, (float)params->extent, 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {params->extent, params->extent}};
        const VkPipelineViewportStateCreateInfo viewport_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
        const VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xfu};
        const VkPipelineColorBlendStateCreateInfo blend = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &blend_attachment};
        /* Subpass 0 keeps the oracle's multisample state without per-sample
         * shading; the fetch subpasses are the single-sample ones. */
        VkSampleMask mask = ps5vk_sample_count_full_mask(params->samples);
        const VkPipelineMultisampleStateCreateInfo multisample[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
             .rasterizationSamples = params->samples, .pSampleMask = &mask},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
             .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT}};
        for (unsigned subpass = 0; subpass < 3; ++subpass) {
            const VkGraphicsPipelineCreateInfo pipeline_info = {
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .layout = layout, .renderPass = pass, .subpass = subpass, .stageCount = 2,
                .pStages = stages[subpass ? 1u : 0u], .pVertexInputState = &vertex_input,
                .pInputAssemblyState = &assembly, .pRasterizationState = &raster,
                .pMultisampleState = &multisample[subpass ? 1u : 0u], .pViewportState = &viewport_state,
                .pColorBlendState = &blend};
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_SAMPLE_RATE_SHAPE step=create_pipeline subpass=%u", subpass);
            rc = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                &pipelines[subpass]);
            if (!shape_step("create_pipeline", rc)) goto cleanup;
        }
    }
    /* Step 9: record the pass the way the oracle records it - clear, draw the
     * multisampled attachment, then one per-sample fetch draw per subpass. */
    {
        const VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
        SHAPE_TRY(vkCreateCommandPool(device, &pool_info, NULL, &command_pool));
        const VkCommandBufferAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        SHAPE_TRY(vkAllocateCommandBuffers(device, &allocate, &command));
        const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        SHAPE_TRY(vkCreateFence(device, &fence_info, NULL, &fence));
        /* Before the oracle's pass, the shape the executor can already be
         * taught to run: two subpasses, EACH with its own colour target, no
         * resolve, no input attachment and no preserve list. It is measured on
         * its own so that "a later subpass renders elsewhere" is a capability
         * with evidence rather than a side effect of the oracle's gate. */
        {
            VkRenderPass two_pass = VK_NULL_HANDLE;
            VkFramebuffer two_fb = VK_NULL_HANDLE;
            VkPipeline two_pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            const VkAttachmentDescription two_attachments[2] = {
                {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = params->samples,
                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                 .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                 .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
            const VkAttachmentReference two_color0 = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            const VkAttachmentReference two_color1 = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            /* The second subpass PRESERVES the first one's target: it renders
             * elsewhere and promises to leave attachment 0 alone, which is
             * what the oracle's fetch subpasses do for their siblings. */
            const uint32_t two_preserve[1] = {0};
            const VkSubpassDescription two_subpasses[2] = {
                {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                 .colorAttachmentCount = 1, .pColorAttachments = &two_color0},
                {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                 .colorAttachmentCount = 1, .pColorAttachments = &two_color1,
                 .preserveAttachmentCount = 1, .pPreserveAttachments = two_preserve}};
            const VkRenderPassCreateInfo two_pass_info = {
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                .attachmentCount = 2, .pAttachments = two_attachments,
                .subpassCount = 2, .pSubpasses = two_subpasses};
            ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_TARGETS step=pass");
            rc = vkCreateRenderPass(device, &two_pass_info, NULL, &two_pass);
            if (!shape_step("create_two_subpass_pass", rc) || rc != VK_SUCCESS) goto two_cleanup;
            const VkImageView two_views[2] = {views[0], views[2]};
            const VkFramebufferCreateInfo two_fb_info = {
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = two_pass,
                .attachmentCount = 2, .pAttachments = two_views,
                .width = params->extent, .height = params->extent, .layers = 1};
            rc = vkCreateFramebuffer(device, &two_fb_info, NULL, &two_fb);
            if (!shape_step("create_two_subpass_framebuffer", rc) || rc != VK_SUCCESS) goto two_cleanup;
            for (unsigned subpass = 0; subpass < 2; ++subpass) {
                VkSampleMask two_mask = ps5vk_sample_count_full_mask(params->samples);
                const VkPipelineShaderStageCreateInfo two_stages[2] = {
                    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", NULL},
                    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, modules[1], "main", NULL}};
                const VkPipelineVertexInputStateCreateInfo two_vertex_input = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                const VkPipelineInputAssemblyStateCreateInfo two_assembly = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
                const VkPipelineRasterizationStateCreateInfo two_raster = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                    .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                    .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
                const VkViewport two_viewport = {0.0f, 0.0f, (float)params->extent,
                    (float)params->extent, 0.0f, 1.0f};
                const VkRect2D two_scissor = {{0, 0}, {params->extent, params->extent}};
                const VkPipelineViewportStateCreateInfo two_viewport_state = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                    .viewportCount = 1, .pViewports = &two_viewport,
                    .scissorCount = 1, .pScissors = &two_scissor};
                const VkPipelineColorBlendAttachmentState two_blend_attachment = {.colorWriteMask = 0xfu};
                const VkPipelineColorBlendStateCreateInfo two_blend = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                    .attachmentCount = 1, .pAttachments = &two_blend_attachment};
                const VkPipelineMultisampleStateCreateInfo two_multisample = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                    .rasterizationSamples = subpass ? VK_SAMPLE_COUNT_1_BIT : params->samples,
                    .pSampleMask = subpass ? NULL : &two_mask};
                const VkGraphicsPipelineCreateInfo two_pipeline_info = {
                    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                    .layout = layout, .renderPass = two_pass, .subpass = subpass, .stageCount = 2,
                    .pStages = two_stages, .pVertexInputState = &two_vertex_input,
                    .pInputAssemblyState = &two_assembly, .pRasterizationState = &two_raster,
                    .pMultisampleState = &two_multisample, .pViewportState = &two_viewport_state,
                    .pColorBlendState = &two_blend};
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_SAMPLE_RATE_TARGETS step=pipeline subpass=%u", subpass);
                rc = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &two_pipeline_info,
                    NULL, &two_pipelines[subpass]);
                if (!shape_step("create_two_subpass_pipeline", rc) || rc != VK_SUCCESS) goto two_cleanup;
            }
            /* The per-sample read (DXVK262-T06): subpass 0 draws with a stage
             * whose output varies with gl_SampleID, so each sample plane holds
             * a value only its own sample produced, and subpass 1 reads ONE
             * named sample of that multisampled attachment through the
             * resource-only record and writes it to its own target. The oracle
             * is the value: sample 1's plane is ff010000, which neither the
             * average nor sample 0 can produce. */
            {
                VkRenderPass fetch_pass = VK_NULL_HANDLE;
                VkFramebuffer fetch_fb = VK_NULL_HANDLE;
                VkPipeline fetch_pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
                VkPipeline fetch_baked = VK_NULL_HANDLE;
                VkDescriptorSetLayout fetch_set_layout = VK_NULL_HANDLE;
                VkDescriptorPool fetch_pool = VK_NULL_HANDLE;
                VkDescriptorSet fetch_set = VK_NULL_HANDLE;
                VkPipelineLayout fetch_layout = VK_NULL_HANDLE;
                VkShaderModule sample_module = VK_NULL_HANDLE;
                VkShaderModule const_module = VK_NULL_HANDLE;
                const VkAttachmentDescription fetch_attachments[2] = {
                    {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = params->samples,
                     .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                     .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                     .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                     .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
                    {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                     .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                     .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                     .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                     .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
                const VkAttachmentReference fetch_color0 = {0, VK_IMAGE_LAYOUT_GENERAL};
                const VkAttachmentReference fetch_input0 = {0, VK_IMAGE_LAYOUT_GENERAL};
                const VkAttachmentReference fetch_color1 = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                const VkSubpassDescription fetch_subpasses[2] = {
                    {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                     .colorAttachmentCount = 1, .pColorAttachments = &fetch_color0},
                    {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                     .inputAttachmentCount = 1, .pInputAttachments = &fetch_input0,
                     .colorAttachmentCount = 1, .pColorAttachments = &fetch_color1}};
                const VkSubpassDependency fetch_dependency = {
                    .srcSubpass = 0, .dstSubpass = 1,
                    .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
                    .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};
                const VkRenderPassCreateInfo fetch_pass_info = {
                    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                    .attachmentCount = 2, .pAttachments = fetch_attachments,
                    .subpassCount = 2, .pSubpasses = fetch_subpasses,
                    .dependencyCount = 1, .pDependencies = &fetch_dependency};
                rc = vkCreateRenderPass(device, &fetch_pass_info, NULL, &fetch_pass);
                if (!shape_step("fetch_create_pass", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                const VkImageView fetch_views[2] = {views[0], views[2]};
                const VkFramebufferCreateInfo fetch_fb_info = {
                    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = fetch_pass,
                    .attachmentCount = 2, .pAttachments = fetch_views,
                    .width = params->extent, .height = params->extent, .layers = 1};
                rc = vkCreateFramebuffer(device, &fetch_fb_info, NULL, &fetch_fb);
                if (!shape_step("fetch_create_framebuffer", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                {
                    const VkDescriptorSetLayoutBinding fetch_bindings[2] = {
                        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
                    const VkDescriptorSetLayoutCreateInfo fetch_set_info = {
                        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                        .bindingCount = 2, .pBindings = fetch_bindings};
                    rc = vkCreateDescriptorSetLayout(device, &fetch_set_info, NULL, &fetch_set_layout);
                    if (!shape_step("fetch_set_layout", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    const VkDescriptorPoolSize fetch_sizes[2] = {
                        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
                    const VkDescriptorPoolCreateInfo fetch_pool_info = {
                        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = fetch_sizes};
                    rc = vkCreateDescriptorPool(device, &fetch_pool_info, NULL, &fetch_pool);
                    if (!shape_step("fetch_pool", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    const VkDescriptorSetAllocateInfo fetch_allocate = {
                        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                        .descriptorPool = fetch_pool, .descriptorSetCount = 1,
                        .pSetLayouts = &fetch_set_layout};
                    rc = vkAllocateDescriptorSets(device, &fetch_allocate, &fetch_set);
                    if (!shape_step("fetch_set", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    const VkDescriptorImageInfo fetch_image = {VK_NULL_HANDLE, views[0],
                        VK_IMAGE_LAYOUT_GENERAL};
                    const VkDescriptorBufferInfo fetch_buffer = {.buffer = ubo, .offset = 0, .range = 4};
                    const VkWriteDescriptorSet fetch_writes[2] = {
                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = fetch_set,
                         .dstBinding = 0, .descriptorCount = 1,
                         .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                         .pImageInfo = &fetch_image},
                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = fetch_set,
                         .dstBinding = 1, .descriptorCount = 1,
                         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         .pBufferInfo = &fetch_buffer}};
                    vkUpdateDescriptorSets(device, 2, fetch_writes, 0, NULL);
                    const VkPipelineLayoutCreateInfo fetch_layout_info = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                        .setLayoutCount = 1, .pSetLayouts = &fetch_set_layout};
                    rc = vkCreatePipelineLayout(device, &fetch_layout_info, NULL, &fetch_layout);
                    if (!shape_step("fetch_layout", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                }
                {
                    const VkShaderModuleCreateInfo sample_info = {
                        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                        .codeSize = params->sample_fragment_words * 4u,
                        .pCode = params->sample_fragment};
                    rc = vkCreateShaderModule(device, &sample_info, NULL, &sample_module);
                    if (!shape_step("fetch_sample_module", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    VkShaderModuleCreateInfo const_info = sample_info;
                    const_info.codeSize = params->fetch_const_fragment_words * 4u;
                    const_info.pCode = params->fetch_const_fragment;
                    rc = vkCreateShaderModule(device, &const_info, NULL, &const_module);
                    if (!shape_step("fetch_const_module", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                }
                for (unsigned phase = 0; phase < 2; ++phase) {
                    VkSampleMask fetch_mask = ps5vk_sample_count_full_mask(params->samples);
                    const VkPipelineShaderStageCreateInfo fetch_stages[2] = {
                        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                         VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", NULL},
                        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                         VK_SHADER_STAGE_FRAGMENT_BIT, phase ? modules[2] : sample_module,
                         "main", NULL}};
                    /* The sweep's SECOND index uses the baked-in module, so a
                     * uniform that never arrived cannot be mistaken for a read
                     * that ignores the index. */
                    const VkPipelineShaderStageCreateInfo fetch_stages_const[2] = {
                        fetch_stages[0],
                        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                         VK_SHADER_STAGE_FRAGMENT_BIT, const_module, "main", NULL}};
                    const VkPipelineVertexInputStateCreateInfo fetch_vertex_input = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                    const VkPipelineInputAssemblyStateCreateInfo fetch_assembly = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
                    const VkPipelineRasterizationStateCreateInfo fetch_raster = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                        .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
                    const VkPipelineMultisampleStateCreateInfo fetch_multisample = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                        .rasterizationSamples = phase ? VK_SAMPLE_COUNT_1_BIT : params->samples,
                        .sampleShadingEnable = phase ? VK_FALSE : VK_TRUE,
                        .minSampleShading = phase ? 0.0f : 1.0f,
                        .pSampleMask = phase ? NULL : &fetch_mask};
                    const VkViewport fetch_viewport = {0.0f, 0.0f, (float)params->extent,
                        (float)params->extent, 0.0f, 1.0f};
                    const VkRect2D fetch_scissor = {{0, 0}, {params->extent, params->extent}};
                    const VkPipelineViewportStateCreateInfo fetch_viewport_state = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                        .viewportCount = 1, .pViewports = &fetch_viewport,
                        .scissorCount = 1, .pScissors = &fetch_scissor};
                    const VkPipelineColorBlendAttachmentState fetch_blend_attachment = {.colorWriteMask = 0xfu};
                    const VkPipelineColorBlendStateCreateInfo fetch_blend = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                        .attachmentCount = 1, .pAttachments = &fetch_blend_attachment};
                    const VkGraphicsPipelineCreateInfo fetch_pipeline_info = {
                        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                        .layout = fetch_layout, .renderPass = fetch_pass, .subpass = phase,
                        .stageCount = 2, .pStages = fetch_stages,
                        .pVertexInputState = &fetch_vertex_input,
                        .pInputAssemblyState = &fetch_assembly,
                        .pRasterizationState = &fetch_raster,
                        .pMultisampleState = &fetch_multisample,
                        .pViewportState = &fetch_viewport_state,
                        .pColorBlendState = &fetch_blend};
                    ps5log_printf(PS5LOG_MARK,
                        "PS5VK_SAMPLE_RATE_FETCH step=pipeline subpass=%u", phase);
                    rc = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &fetch_pipeline_info,
                        NULL, &fetch_pipelines[phase]);
                    if (!shape_step("fetch_create_pipeline", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    if (phase) {
                        VkGraphicsPipelineCreateInfo baked_info = fetch_pipeline_info;
                        baked_info.pStages = fetch_stages_const;
                        rc = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &baked_info,
                            NULL, &fetch_baked);
                        if (!shape_step("fetch_baked_pipeline", rc) || rc != VK_SUCCESS)
                            goto fetch_cleanup;
                    }
                }
                /* The uniform block names the sample the fetch stage reads, so
                 * the phase is a SWEEP over sample indices: one submit per
                 * index, each judged on its own readback. Two indices are
                 * enough to tell "the index is not delivered" (every index
                 * returns the same plane) from "the read is fixed to plane
                 * zero" and from a working per-sample read. */
                for (unsigned pass_index = 0; pass_index < 2; ++pass_index) {
                    const int32_t wanted = pass_index ?
                        (int32_t)(ps5vk_sample_count_number(params->samples) - 1u) : 0;
                    if (!ubo_mapped) goto fetch_cleanup;
                    memcpy(ubo_mapped, &wanted, sizeof(wanted));
                    /* The uniform block is non-coherent host memory: the GPU
                     * only sees it after the driver flushes the range, exactly
                     * as the readback side invalidates before reading. */
                    {
                        VkMappedMemoryRange ubo_range = {
                            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                            .memory = ubo_memory, .offset = 0, .size = VK_WHOLE_SIZE};
                        rc = vkFlushMappedMemoryRanges(device, 1, &ubo_range);
                        if (!shape_step("fetch_ubo_flush", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    }
                    /* The clear has to be a value NO sample can hold: the
                     * sample-id module writes 0xff0k0000, and a clear of
                     * {0,0,0,1} is exactly 0xff000000 - indistinguishable from
                     * sample zero, which would make a fetch that wrote nothing
                     * look like a fetch that read plane zero. */
                    VkClearValue fetch_clears[2] = {
                        {.color = {.float32 = {0.5f, 0.5f, 0.5f, 1.0f}}},
                        {.color = {.float32 = {0.5f, 0.5f, 0.5f, 1.0f}}}};
                    const VkRenderPassBeginInfo fetch_begin_info = {
                        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = fetch_pass,
                        .framebuffer = fetch_fb,
                        .renderArea = {{0, 0}, {params->extent, params->extent}},
                        .clearValueCount = 2, .pClearValues = fetch_clears};
                    const VkCommandBufferBeginInfo fetch_begin = {
                        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    rc = vkBeginCommandBuffer(command, &fetch_begin);
                    if (!shape_step("fetch_begin_command", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    vkCmdBeginRenderPass(command, &fetch_begin_info, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, fetch_pipelines[0]);
                    vkCmdDraw(command, 3, 1, 0, 0);
                    vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pass_index ? fetch_baked : fetch_pipelines[1]);
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, fetch_layout, 0,
                        1, &fetch_set, 0, NULL);
                    vkCmdDraw(command, 3, 1, 0, 0);
                    vkCmdEndRenderPass(command);
                    rc = vkEndCommandBuffer(command);
                    if (!shape_step("fetch_end_command", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    VkQueue fetch_queue = VK_NULL_HANDLE;
                    vkGetDeviceQueue(device, 0, 0, &fetch_queue);
                    const VkSubmitInfo fetch_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &command};
                    rc = vkQueueSubmit(fetch_queue, 1, &fetch_submit, fence);
                    if (!shape_step("fetch_submit", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    rc = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000));
                    if (!shape_step("fetch_wait", rc) || rc != VK_SUCCESS) goto fetch_cleanup;
                    rc = vkResetFences(device, 1, &fence);
                    void *fetched = NULL;
                    VkDeviceSize fetched_bytes = 0;
                    VkResult map_rc = ps5vk_image_span(device, images[2], &fetched, &fetched_bytes);
                    if (!shape_step("fetch_readback_map", map_rc) || map_rc != VK_SUCCESS)
                        goto fetch_cleanup;
                    /* The value a plane holds is R = sampleID/255: the byte
                     * is the sample id itself, which the 32x32 source census
                     * measures directly (sample 1 -> ff000001, sample 2 ->
                     * ff000002, sample 3 -> ff000003); the 64x64 witness wrote
                     * the same ids through a different format path. */
                    const uint32_t wanted_word = UINT32_C(0xff000000) | (uint32_t)wanted;
                    const uint32_t words = (uint32_t)(fetched_bytes / 4u);
                    const uint32_t plane_words = params->extent * params->extent;
                    uint32_t matched = 0;
                    uint32_t seen[4] = {0};
                    unsigned distinct = 0;
                    for (uint32_t i = 0; i < words; ++i) {
                        uint32_t word = 0;
                        memcpy(&word, (const unsigned char *)fetched + (size_t)i * 4u, sizeof(word));
                        if (word == wanted_word) ++matched;
                        unsigned found = 0;
                        for (unsigned k = 0; k < distinct && !found; ++k) found = seen[k] == word;
                        if (!found && distinct < 4u) seen[distinct++] = word;
                    }
                    ps5log_printf(PS5LOG_MARK,
                        "PS5VK_SAMPLE_RATE_FETCH extent=%ux%u samples=%u sample_index=%d words=%u "
                        "matched=%u expected=%u value=%08x seen=%08x,%08x,%08x,%08x verdict=%u",
                        params->extent, params->extent, (unsigned)params->samples, (int)wanted, words,
                        matched, plane_words, wanted_word,
                        seen[0], seen[1], seen[2], seen[3],
                        (unsigned)(matched == plane_words));
                }
                /* After the sweep, the SOURCE's own layout is measured: the
                 * multisampled attachment is mapped through the driver's span
                 * after subpass 0 dressed every sample differently, and each
                 * sample value's word count and first/last offset are logged, now that
                 * subpass 0 has drawn them in the last iteration.
                 * What the hardware does with the storage it was given is the
                 * fact that decides what a sample-indexed read should address;
                 * assuming the planes are stacked is what the last window got
                 * wrong. */
                /* The resolve arithmetic, measured: one more submit of the same
                 * pass whose fetch stage is replaced by a stage that reads
                 * EVERY sample and writes their average. The oracle is that the
                 * target holds a value NONE of the samples had - an average, not
                 * a plane - which is what tells a resolve apart from a copy. */
                {
                    VkShaderModule resolve_module = VK_NULL_HANDLE;
                    VkPipeline resolve_pipeline = VK_NULL_HANDLE;
                    VkResult resolve_rc = vkCreateShaderModule(device,
                        &(VkShaderModuleCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                            .codeSize = params->resolve_fragment_words * 4u,
                            .pCode = params->resolve_fragment},
                        NULL, &resolve_module);
                    if (!shape_step("resolve_module", resolve_rc) || resolve_rc != VK_SUCCESS)
                        goto resolve_done;
                    {
                        const VkPipelineShaderStageCreateInfo resolve_stages[2] = {
                            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                             VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", NULL},
                            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                             VK_SHADER_STAGE_FRAGMENT_BIT, resolve_module, "main", NULL}};
                        const VkPipelineVertexInputStateCreateInfo resolve_vertex_input = {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                        const VkPipelineInputAssemblyStateCreateInfo resolve_assembly = {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
                        const VkPipelineRasterizationStateCreateInfo resolve_raster = {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                            .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
                        /* The resolve draw RENDERS INTO a single-sample target,
                         * so its own rasterization is single-sample: the
                         * multisampling it reads belongs to the attachment it
                         * reads, and the front end refuses a pipeline whose
                         * sample count does not match the subpass it renders
                         * in. */
                        const VkPipelineMultisampleStateCreateInfo resolve_multisample = {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
                        const VkViewport resolve_viewport = {0.0f, 0.0f, (float)params->extent,
                            (float)params->extent, 0.0f, 1.0f};
                        const VkRect2D resolve_scissor = {{0, 0}, {params->extent, params->extent}};
                        const VkPipelineViewportStateCreateInfo resolve_viewport_state = {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                            .viewportCount = 1, .pViewports = &resolve_viewport,
                            .scissorCount = 1, .pScissors = &resolve_scissor};
                        const VkPipelineColorBlendAttachmentState resolve_blend_attachment = {
                            .colorWriteMask = 0xfu};
                        const VkPipelineColorBlendStateCreateInfo resolve_blend = {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                            .attachmentCount = 1, .pAttachments = &resolve_blend_attachment};
                        const VkGraphicsPipelineCreateInfo resolve_info = {
                            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                            .layout = fetch_layout, .renderPass = fetch_pass, .subpass = 1,
                            .stageCount = 2, .pStages = resolve_stages,
                            .pVertexInputState = &resolve_vertex_input,
                            .pInputAssemblyState = &resolve_assembly,
                            .pRasterizationState = &resolve_raster,
                            .pMultisampleState = &resolve_multisample,
                            .pViewportState = &resolve_viewport_state,
                            .pColorBlendState = &resolve_blend};
                        resolve_rc = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                            &resolve_info, NULL, &resolve_pipeline);
                        if (!shape_step("resolve_pipeline", resolve_rc) || resolve_rc != VK_SUCCESS)
                            goto resolve_done;
                    }
                    {
                        VkClearValue resolve_clears[2] = {
                            {.color = {.float32 = {0.5f, 0.5f, 0.5f, 1.0f}}},
                            {.color = {.float32 = {0.5f, 0.5f, 0.5f, 1.0f}}}};
                        const VkRenderPassBeginInfo resolve_begin_info = {
                            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                            .renderPass = fetch_pass, .framebuffer = fetch_fb,
                            .renderArea = {{0, 0}, {params->extent, params->extent}},
                            .clearValueCount = 2, .pClearValues = resolve_clears};
                        resolve_rc = vkBeginCommandBuffer(command,
                            &(VkCommandBufferBeginInfo){
                                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
                        if (!shape_step("resolve_begin_command", resolve_rc) || resolve_rc != VK_SUCCESS)
                            goto resolve_done;
                        vkCmdBeginRenderPass(command, &resolve_begin_info, VK_SUBPASS_CONTENTS_INLINE);
                        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, fetch_pipelines[0]);
                        vkCmdDraw(command, 3, 1, 0, 0);
                        vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
                        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline);
                        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, fetch_layout,
                            0, 1, &fetch_set, 0, NULL);
                        vkCmdDraw(command, 3, 1, 0, 0);
                        vkCmdEndRenderPass(command);
                        resolve_rc = vkEndCommandBuffer(command);
                        if (!shape_step("resolve_end_command", resolve_rc) || resolve_rc != VK_SUCCESS)
                            goto resolve_done;
                        VkQueue resolve_queue = VK_NULL_HANDLE;
                        vkGetDeviceQueue(device, 0, 0, &resolve_queue);
                        const VkSubmitInfo resolve_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &command};
                        resolve_rc = vkQueueSubmit(resolve_queue, 1, &resolve_submit, fence);
                        if (!shape_step("resolve_submit", resolve_rc) || resolve_rc != VK_SUCCESS)
                            goto resolve_done;
                        resolve_rc = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000));
                        if (!shape_step("resolve_wait", resolve_rc) || resolve_rc != VK_SUCCESS)
                            goto resolve_done;
                        resolve_rc = vkResetFences(device, 1, &fence);
                    }
                    {
                        void *resolved = NULL;
                        VkDeviceSize resolved_bytes = 0;
                        VkResult map_rc = ps5vk_image_span(device, images[2], &resolved, &resolved_bytes);
                        if (!shape_step("resolve_readback_map", map_rc) || map_rc != VK_SUCCESS)
                            goto resolve_done;
                        const uint32_t words = (uint32_t)(resolved_bytes / 4u);
                        uint32_t seen[4] = {0};
                        unsigned distinct = 0;
                        for (uint32_t i = 0; i < words; ++i) {
                            uint32_t word = 0;
                            memcpy(&word, (const unsigned char *)resolved + (size_t)i * 4u,
                                sizeof(word));
                            unsigned found = 0;
                            for (unsigned k = 0; k < distinct && !found; ++k) found = seen[k] == word;
                            if (!found && distinct < 4u) seen[distinct++] = word;
                        }
                        /* The four samples held R = 0,1,2,3; the average is 1.5,
                         * so the result is neither a sample nor the clear. */
                        /* The samples held R = 0,1,2,3 in the FIRST byte (the
                         * census' encoding); 1.5 rounds to either neighbour,
                         * and what matters is that the result is neither a
                         * sample nor the clear. */
                        const int averaged = (seen[0] == UINT32_C(0xff000001) ||
                                              seen[0] == UINT32_C(0xff000002));
                        ps5log_printf(PS5LOG_MARK,
                            "PS5VK_SAMPLE_RATE_RESOLVE extent=%ux%u samples=%u words=%u distinct=%u "
                            "value=%08x second=%08x averaged=%d",
                            params->extent, params->extent, (unsigned)params->samples, words,
                            distinct, seen[0], seen[1], averaged);
                    }
resolve_done:
                    if (resolve_pipeline) vkDestroyPipeline(device, resolve_pipeline, NULL);
                    if (resolve_module) vkDestroyShaderModule(device, resolve_module, NULL);
                }
                {
                    void *source = NULL;
                    VkDeviceSize source_bytes = 0;
                    VkResult map_rc = ps5vk_image_span(device, images[0], &source, &source_bytes);
                    if (!shape_step("fetch_source_map", map_rc) || map_rc != VK_SUCCESS)
                        goto fetch_cleanup;
                    const uint32_t words = (uint32_t)(source_bytes / 4u);
                    /* What the multisampled attachment ACTUALLY holds after the
                     * last sweep iteration, as a census rather than a search
                     * for expected values: the previous form assumed the four
                     * per-sample encodings were there, and the pass may have
                     * shaded once per pixel instead - in which case there is no
                     * other sample to read and "index 3 returns sample 0" says
                     * nothing about the index at all. */
                    uint32_t census[4] = {0}, census_hits[4] = {0};
                    uint32_t census_first[4] = {0}, census_last[4] = {0};
                    unsigned distinct = 0;
                    for (uint32_t i = 0; i < words; ++i) {
                        uint32_t word = 0;
                        memcpy(&word, (const unsigned char *)source + (size_t)i * 4u, sizeof(word));
                        unsigned slot = distinct;
                        for (unsigned k = 0; k < distinct; ++k) {
                            if (census[k] == word) { slot = k; break; }
                        }
                        if (slot == distinct) {
                            if (distinct == 4u) continue;
                            census[distinct] = word;
                            census_first[distinct] = i;
                            ++distinct;
                        }
                        ++census_hits[slot];
                        census_last[slot] = i;
                    }
                    ps5log_printf(PS5LOG_MARK,
                        "PS5VK_SAMPLE_RATE_SOURCE extent=%ux%u samples=%u words=%u distinct=%u "
                        "value0=%08x hits0=%u first0=%u last0=%u value1=%08x hits1=%u "
                        "value2=%08x hits2=%u value3=%08x hits3=%u",
                        params->extent, params->extent, (unsigned)params->samples, words, distinct,
                        census[0], census_hits[0], census_first[0], census_last[0],
                        census[1], census_hits[1], census[2], census_hits[2],
                        census[3], census_hits[3]);
                }
fetch_cleanup:
                if (sample_module) vkDestroyShaderModule(device, sample_module, NULL);
                for (unsigned i = 0; i < 2; ++i)
                    if (fetch_pipelines[i]) vkDestroyPipeline(device, fetch_pipelines[i], NULL);
                if (fetch_baked) vkDestroyPipeline(device, fetch_baked, NULL);
                if (const_module) vkDestroyShaderModule(device, const_module, NULL);
                if (fetch_layout) vkDestroyPipelineLayout(device, fetch_layout, NULL);
                if (fetch_pool) vkDestroyDescriptorPool(device, fetch_pool, NULL);
                if (fetch_set_layout) vkDestroyDescriptorSetLayout(device, fetch_set_layout, NULL);
                if (fetch_fb) vkDestroyFramebuffer(device, fetch_fb, NULL);
                if (fetch_pass) vkDestroyRenderPass(device, fetch_pass, NULL);
            }
            {
                const VkCommandBufferBeginInfo two_begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                VkClearValue two_clears[2] = {
                    {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}},
                    {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}}};
                const VkRenderPassBeginInfo two_pass_begin = {
                    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = two_pass,
                    .framebuffer = two_fb, .renderArea = {{0, 0}, {params->extent, params->extent}},
                    .clearValueCount = 2, .pClearValues = two_clears};
                rc = vkBeginCommandBuffer(command, &two_begin);
                if (!shape_step("two_subpass_begin_command", rc) || rc != VK_SUCCESS) goto two_cleanup;
                ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_TARGETS step=record");
                vkCmdBeginRenderPass(command, &two_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, two_pipelines[0]);
                vkCmdDraw(command, 3, 1, 0, 0);
                vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, two_pipelines[1]);
                vkCmdDraw(command, 3, 1, 0, 0);
                vkCmdEndRenderPass(command);
                rc = vkEndCommandBuffer(command);
                if (!shape_step("two_subpass_end_command", rc) || rc != VK_SUCCESS) goto two_cleanup;
                VkQueue two_queue = VK_NULL_HANDLE;
                vkGetDeviceQueue(device, 0, 0, &two_queue);
                const VkSubmitInfo two_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1, .pCommandBuffers = &command};
                rc = vkQueueSubmit(two_queue, 1, &two_submit, fence);
                if (!shape_step("two_subpass_submit", rc) || rc != VK_SUCCESS) goto two_cleanup;
                rc = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000));
                if (!shape_step("two_subpass_wait", rc) || rc != VK_SUCCESS) goto two_cleanup;
                rc = vkResetFences(device, 1, &fence);
                /* The oracle: the SECOND subpass's target holds the fragment's
                 * colour across the plane it covers, so a subpass that renders
                 * into its own attachment really rendered. The plane is
                 * extent*extent RGBA8 words; the tiled padding around it keeps
                 * the clear. */
                {
                    void *target = NULL;
                    VkDeviceSize target_bytes = 0;
                    VkResult map_rc = ps5vk_image_span(device, images[2], &target, &target_bytes);
                    if (!shape_step("two_subpass_readback_map", map_rc) || map_rc != VK_SUCCESS)
                        goto two_cleanup;
                    uint32_t expected_word = 0;
                    if (!ps5vk_color_clear_rgba8((const float[]){0.25f, 0.5f, 0.75f, 1.0f},
                            &expected_word))
                        goto two_cleanup;
                    const uint32_t words = (uint32_t)(target_bytes / 4u);
                    uint32_t shaded = 0;
                    for (uint32_t i = 0; i < words; ++i) {
                        uint32_t word = 0;
                        memcpy(&word, (const unsigned char *)target + (size_t)i * 4u, sizeof(word));
                        if (word == expected_word) ++shaded;
                    }
                    const uint32_t plane_words = params->extent * params->extent;
                    /* The PRESERVED attachment: subpass 1 promised not to
                     * touch attachment 0, so the colour subpass 0 drew must
                     * still be somewhere in its span. A subpass that clobbered
                     * it would have left the pass's own clear value there. */
                    void *preserved = NULL;
                    VkDeviceSize preserved_bytes = 0;
                    VkResult preserve_rc = ps5vk_image_span(device, images[0], &preserved,
                        &preserved_bytes);
                    if (!shape_step("two_subpass_preserved_map", preserve_rc) ||
                        preserve_rc != VK_SUCCESS) goto two_cleanup;
                    const uint32_t preserved_words = (uint32_t)(preserved_bytes / 4u);
                    uint32_t preserved_hits = 0;
                    for (uint32_t i = 0; i < preserved_words; ++i) {
                        uint32_t word = 0;
                        memcpy(&word, (const unsigned char *)preserved + (size_t)i * 4u,
                            sizeof(word));
                        if (word == expected_word) ++preserved_hits;
                    }
                    ps5log_printf(PS5LOG_MARK,
                        "PS5VK_SAMPLE_RATE_TARGETS extent=%ux%u samples=%u subpasses=2 "
                        "target_words=%u shaded=%u expected=%u word=%08x "
                        "preserved_words=%u preserved_hits=%u verdict=%u",
                        params->extent, params->extent, (unsigned)params->samples,
                        words, shaded, plane_words, expected_word,
                        preserved_words, preserved_hits,
                        (unsigned)(shaded == plane_words && preserved_hits > 0u));
                    rc = (shaded == plane_words && preserved_hits > 0u) ?
                        VK_SUCCESS : VK_ERROR_UNKNOWN;
                    if (rc != VK_SUCCESS) { goto two_cleanup; }
                }
            }
two_cleanup:
            for (unsigned i = 0; i < 2; ++i)
                if (two_pipelines[i]) vkDestroyPipeline(device, two_pipelines[i], NULL);
            if (two_fb) vkDestroyFramebuffer(device, two_fb, NULL);
            if (two_pass) vkDestroyRenderPass(device, two_pass, NULL);
        }
        const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        SHAPE_TRY(vkBeginCommandBuffer(command, &begin));
        /* One clear value per attachment, as the oracle passes: the pass
         * clears all four, and a shorter array is a malformed begin rather
         * than a shape the driver refuses. */
        const uint32_t clear_count = 4;
        VkClearValue clears[4];
        for (uint32_t i = 0; i < clear_count; ++i)
            clears[i] = (VkClearValue){.color = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}}};
        const VkRenderPassBeginInfo pass_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
            .framebuffer = framebuffer, .renderArea = {{0, 0}, {params->extent, params->extent}},
            .clearValueCount = clear_count, .pClearValues = clears};
        /* Every recorded call is announced too. Recording in this profile
         * poisons the command buffer on a refusal and reports it at
         * vkEndCommandBuffer, so the step that names the refusal is not the
         * call that made it - the call that never logged its successor is. */
        ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=record:vkCmdBeginRenderPass");
        vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=record:vkCmdBindPipeline subpass=0");
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);
        ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=record:vkCmdDraw subpass=0");
        vkCmdDraw(command, 3, 1, 0, 0);
        for (unsigned subpass = 1; subpass < 3; ++subpass) {
            ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=record:vkCmdNextSubpass");
            vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
            ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=record:vkCmdBindPipeline subpass=1");
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[subpass]);
            ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=record:vkCmdBindDescriptorSets");
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
            ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=record:vkCmdDraw subpass=1");
            vkCmdDraw(command, 3, 1, 0, 0);
        }
        ps5log_line(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=record:vkCmdEndRenderPass");
        vkCmdEndRenderPass(command);
        SHAPE_TRY(vkEndCommandBuffer(command));
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, 0, 0, &queue);
        const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        SHAPE_TRY(vkQueueSubmit(queue, 1, &submit, fence));
        SHAPE_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
        SHAPE_TRY(vkDeviceWaitIdle(device));
        shape_step("submitted", VK_SUCCESS);
        /* The oracle's own pass now runs end to end, so its resolve target is
         * read back: the resolved result has to LAND in the attachment the pass
         * declares, whatever value the samples happen to hold. */
        {
            void *resolved = NULL;
            VkDeviceSize resolved_bytes = 0;
            VkResult map_rc = ps5vk_image_span(device, images[1], &resolved, &resolved_bytes);
            if (shape_step("resolve_target_map", map_rc) && map_rc == VK_SUCCESS) {
                const uint32_t words = (uint32_t)(resolved_bytes / 4u);
                uint32_t census[4] = {0}, hits[4] = {0};
                unsigned distinct = 0;
                for (uint32_t i = 0; i < words; ++i) {
                    uint32_t word = 0;
                    memcpy(&word, (const unsigned char *)resolved + (size_t)i * 4u, sizeof(word));
                    unsigned slot = distinct;
                    for (unsigned k = 0; k < distinct; ++k)
                        if (census[k] == word) { slot = k; break; }
                    if (slot == distinct) {
                        if (distinct == 4u) continue;
                        census[distinct] = word;
                        ++distinct;
                    }
                    ++hits[slot];
                }
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_SAMPLE_RATE_RESOLVED extent=%ux%u words=%u distinct=%u "
                    "value0=%08x hits0=%u value1=%08x hits1=%u value2=%08x hits2=%u "
                    "value3=%08x hits3=%u",
                    params->extent, params->extent, words, distinct,
                    census[0], hits[0], census[1], hits[1], census[2], hits[2],
                    census[3], hits[3]);
            }
        }
    }

cleanup:
    /* The teardown is walked too, and each call is announced BEFORE it runs so
     * a destroy path that does not return names itself instead of hiding behind
     * the last API that did. A refusal above leaves objects in states the
     * destroy paths have to accept - the command buffer is still recording, for
     * one - and that is exactly where a fail-closed defect can hide. */
#define SHAPE_CLEANUP(call) do { \
    ps5log_printf(PS5LOG_MARK, "PS5VK_SAMPLE_RATE_SHAPE step=cleanup:%s", #call); \
    (call); \
} while (0)
    if (ubo_mapped) SHAPE_CLEANUP(vkUnmapMemory(device, ubo_memory));
    if (fence) SHAPE_CLEANUP(vkDestroyFence(device, fence, NULL));
    if (command) SHAPE_CLEANUP(vkFreeCommandBuffers(device, command_pool, 1, &command));
    if (command_pool) SHAPE_CLEANUP(vkDestroyCommandPool(device, command_pool, NULL));
    for (unsigned i = 0; i < 3; ++i)
        if (pipelines[i]) SHAPE_CLEANUP(vkDestroyPipeline(device, pipelines[i], NULL));
    for (unsigned i = 0; i < 3; ++i)
        if (modules[i]) SHAPE_CLEANUP(vkDestroyShaderModule(device, modules[i], NULL));
    if (layout) SHAPE_CLEANUP(vkDestroyPipelineLayout(device, layout, NULL));
    if (pool) SHAPE_CLEANUP(vkDestroyDescriptorPool(device, pool, NULL));
    if (set_layout) SHAPE_CLEANUP(vkDestroyDescriptorSetLayout(device, set_layout, NULL));
    if (ubo) SHAPE_CLEANUP(vkDestroyBuffer(device, ubo, NULL));
    if (ubo_memory) SHAPE_CLEANUP(vkFreeMemory(device, ubo_memory, NULL));
    if (framebuffer) SHAPE_CLEANUP(vkDestroyFramebuffer(device, framebuffer, NULL));
    if (pass) SHAPE_CLEANUP(vkDestroyRenderPass(device, pass, NULL));
    for (unsigned i = 0; i < 4; ++i) {
        if (views[i]) SHAPE_CLEANUP(vkDestroyImageView(device, views[i], NULL));
        if (images[i]) SHAPE_CLEANUP(vkDestroyImage(device, images[i], NULL));
        if (memories[i]) SHAPE_CLEANUP(vkFreeMemory(device, memories[i], NULL));
    }
#undef SHAPE_CLEANUP
    return VK_SUCCESS;
#undef SHAPE_TRY
}

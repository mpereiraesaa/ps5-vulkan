/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public-SDK-only witness for two cubes and all twelve sampled faces.
 * Included after CHECK/REQUIRE, ps5log and the generated shader arrays.
 */
#include "cube_array_witness_shaders.h"

#ifndef CONSUMER_CUBE_ARRAY_BASE_LAYER
#define CONSUMER_CUBE_ARRAY_BASE_LAYER 0
#endif
#ifndef CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
#define CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT 0
#endif

enum {
    CUBE_ARRAY_FACE_COUNT = 6,
    CUBE_ARRAY_CUBE_COUNT = 2,
    CUBE_ARRAY_LAYER_COUNT = CUBE_ARRAY_FACE_COUNT * CUBE_ARRAY_CUBE_COUNT,
    CUBE_ARRAY_STORAGE_LAYERS = CUBE_ARRAY_LAYER_COUNT + CONSUMER_CUBE_ARRAY_BASE_LAYER,
    CUBE_ARRAY_FACE_EXTENT = CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT ? 256 : 4,
    CUBE_ARRAY_CELL_EXTENT = 16,
    CUBE_ARRAY_TARGET_WIDTH = CUBE_ARRAY_LAYER_COUNT * CUBE_ARRAY_CELL_EXTENT,
    CUBE_ARRAY_TARGET_HEIGHT = 64,
    CUBE_ARRAY_PIXEL_BYTES = 4,
    CUBE_ARRAY_FACE_BYTES = CUBE_ARRAY_FACE_EXTENT * CUBE_ARRAY_FACE_EXTENT *
                            CUBE_ARRAY_PIXEL_BYTES,
};

static void cube_array_expected_color(unsigned cell, uint8_t out[4])
{
    out[0] = (uint8_t)(17u * (cell + 1u));
    out[1] = (uint8_t)(255u - 17u * cell);
    out[2] = (uint8_t)(19u + 17u * cell);
    out[3] = 255;
}

static VkDeviceMemory cube_array_allocate(VkDevice device, VkDeviceSize size)
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = size,
        .memoryTypeIndex = 0,
    };
    CHECK(vkAllocateMemory(device, &allocation, NULL, &memory));
    return memory;
}

static void run_cube_array_witness(VkPhysicalDevice physical, VkDevice device,
                                   VkQueue queue)
{
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_CUBE_ARRAY_START cubes=%u faces=%u layers=%u image=%ux%u"
        " target=%ux%u storage_layers=%u base_array_layer=%u"
        " vertex_sha256=%s fragment_sha256=%s",
        CUBE_ARRAY_CUBE_COUNT, CUBE_ARRAY_FACE_COUNT, CUBE_ARRAY_LAYER_COUNT,
        CUBE_ARRAY_FACE_EXTENT, CUBE_ARRAY_FACE_EXTENT,
        CUBE_ARRAY_TARGET_WIDTH, CUBE_ARRAY_TARGET_HEIGHT,
        CUBE_ARRAY_STORAGE_LAYERS, CONSUMER_CUBE_ARRAY_BASE_LAYER,
        CONSUMER_CUBE_ARRAY_VERT_SPIRV_SHA256,
        CONSUMER_CUBE_ARRAY_FRAG_SPIRV_SHA256);
#if CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_CUBE_ARRAY_SOURCE kind=tiled_attachment rendered_layers=%u",
        CUBE_ARRAY_STORAGE_LAYERS);
#endif

    VkFormatProperties format_properties = {0};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                        &format_properties);
    const VkFormatFeatureFlags sampled_required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    REQUIRE((format_properties.optimalTilingFeatures & sampled_required) == sampled_required,
            "cube-array color format reports sampled and transfer-destination support");
    const VkFormatFeatureFlags target_required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    REQUIRE((format_properties.optimalTilingFeatures & target_required) == target_required,
            "cube-array witness target reports attachment and transfer-source support");

    VkImage cube_image = VK_NULL_HANDLE;
    VkDeviceMemory cube_memory = VK_NULL_HANDLE;
    VkImageView cube_view = VK_NULL_HANDLE;
    VkImageCreateInfo cube_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {CUBE_ARRAY_FACE_EXTENT, CUBE_ARRAY_FACE_EXTENT, 1},
        .mipLevels = 1,
        .arrayLayers = CUBE_ARRAY_STORAGE_LAYERS,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
#if CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
#else
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
#endif
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(device, &cube_info, NULL, &cube_image));
    VkMemoryRequirements cube_requirements = {0};
    vkGetImageMemoryRequirements(device, cube_image, &cube_requirements);
    cube_memory = cube_array_allocate(device, cube_requirements.size);
    CHECK(vkBindImageMemory(device, cube_image, cube_memory, 0));
    VkImageViewCreateInfo cube_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = cube_image,
        .viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                             CONSUMER_CUBE_ARRAY_BASE_LAYER, CUBE_ARRAY_LAYER_COUNT},
    };
    CHECK(vkCreateImageView(device, &cube_view_info, NULL, &cube_view));

    VkBuffer upload_buffer = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE;
    VkBufferCreateInfo upload_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = CUBE_ARRAY_STORAGE_LAYERS * CUBE_ARRAY_FACE_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(device, &upload_info, NULL, &upload_buffer));
    VkMemoryRequirements upload_requirements = {0};
    vkGetBufferMemoryRequirements(device, upload_buffer, &upload_requirements);
    upload_memory = cube_array_allocate(device, upload_requirements.size);
    CHECK(vkBindBufferMemory(device, upload_buffer, upload_memory, 0));
    uint8_t *upload_bytes = NULL;
    CHECK(vkMapMemory(device, upload_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&upload_bytes));
    for (unsigned layer = 0; layer < CUBE_ARRAY_STORAGE_LAYERS; ++layer) {
        uint8_t rgba[4];
        /* The excluded prefix has a distinct color: a descriptor that ignores
         * the view base cannot accidentally reproduce the expected faces. */
        cube_array_expected_color(layer, rgba);
        for (unsigned pixel = 0; pixel < CUBE_ARRAY_FACE_EXTENT * CUBE_ARRAY_FACE_EXTENT;
             ++pixel)
            memcpy(upload_bytes + layer * CUBE_ARRAY_FACE_BYTES + pixel * 4u,
                   rgba, sizeof(rgba));
    }
    VkMappedMemoryRange upload_flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = upload_memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    CHECK(vkFlushMappedMemoryRanges(device, 1, &upload_flush));
    vkUnmapMemory(device, upload_memory);

    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    };
    CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler));
    VkDescriptorSetLayoutBinding descriptor_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &descriptor_binding,
    };
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(device, &descriptor_layout_info, NULL,
                                      &descriptor_layout));
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo descriptor_allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_layout,
    };
    CHECK(vkAllocateDescriptorSets(device, &descriptor_allocate, &descriptor_set));
    VkDescriptorImageInfo descriptor_image = {
        .sampler = sampler,
        .imageView = cube_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet descriptor_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &descriptor_image,
    };
    vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, NULL);

    VkImage target_image = VK_NULL_HANDLE;
    VkDeviceMemory target_memory = VK_NULL_HANDLE;
    VkImageView target_view = VK_NULL_HANDLE;
    VkImageCreateInfo target_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {CUBE_ARRAY_TARGET_WIDTH, CUBE_ARRAY_TARGET_HEIGHT, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(device, &target_info, NULL, &target_image));
    VkMemoryRequirements target_requirements = {0};
    vkGetImageMemoryRequirements(device, target_image, &target_requirements);
    target_memory = cube_array_allocate(device, target_requirements.size);
    CHECK(vkBindImageMemory(device, target_image, target_memory, 0));
    VkImageViewCreateInfo target_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = target_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    CHECK(vkCreateImageView(device, &target_view_info, NULL, &target_view));
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkBufferCreateInfo readback_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = CUBE_ARRAY_TARGET_WIDTH * CUBE_ARRAY_TARGET_HEIGHT *
                CUBE_ARRAY_PIXEL_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(device, &readback_info, NULL, &readback_buffer));
    VkMemoryRequirements readback_requirements = {0};
    vkGetBufferMemoryRequirements(device, readback_buffer, &readback_requirements);
    readback_memory = cube_array_allocate(device, readback_requirements.size);
    CHECK(vkBindBufferMemory(device, readback_buffer, readback_memory, 0));
    uint8_t *readback_bytes = NULL;
    CHECK(vkMapMemory(device, readback_memory, 0, readback_requirements.size, 0,
                      (void **)&readback_bytes));

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };
    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };
    VkRenderPass render_pass = VK_NULL_HANDLE;
    CHECK(vkCreateRenderPass(device, &render_pass_info, NULL, &render_pass));
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_pass,
        .attachmentCount = 1,
        .pAttachments = &target_view,
        .width = CUBE_ARRAY_TARGET_WIDTH,
        .height = CUBE_ARRAY_TARGET_HEIGHT,
        .layers = 1,
    };
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));
#if CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
    VkImageView face_views[CUBE_ARRAY_STORAGE_LAYERS] = {0};
    VkFramebuffer face_framebuffers[CUBE_ARRAY_STORAGE_LAYERS] = {0};
    for (unsigned layer = 0; layer < CUBE_ARRAY_STORAGE_LAYERS; ++layer) {
        VkImageViewCreateInfo face_view_info = cube_view_info;
        face_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        face_view_info.subresourceRange.baseArrayLayer = layer;
        face_view_info.subresourceRange.layerCount = 1;
        CHECK(vkCreateImageView(device, &face_view_info, NULL, &face_views[layer]));
        VkFramebufferCreateInfo face_framebuffer_info = framebuffer_info;
        face_framebuffer_info.pAttachments = &face_views[layer];
        face_framebuffer_info.width = CUBE_ARRAY_FACE_EXTENT;
        face_framebuffer_info.height = CUBE_ARRAY_FACE_EXTENT;
        CHECK(vkCreateFramebuffer(device, &face_framebuffer_info, NULL,
                                  &face_framebuffers[layer]));
    }
#endif

    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_cube_array_vert_spirv),
        .pCode = consumer_cube_array_vert_spirv,
    };
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &vertex_module));
    shader_info.codeSize = sizeof(consumer_cube_array_frag_spirv);
    shader_info.pCode = consumer_cube_array_frag_spirv;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &fragment_module));
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_layout,
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_module, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_module, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = {0.0f, 0.0f, CUBE_ARRAY_TARGET_WIDTH,
                           CUBE_ARRAY_TARGET_HEIGHT, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {CUBE_ARRAY_TARGET_WIDTH, CUBE_ARRAY_TARGET_HEIGHT}};
    VkPipelineViewportStateCreateInfo viewport_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                    NULL, &pipeline));

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0,
    };
    CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo command_allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CHECK(vkAllocateCommandBuffers(device, &command_allocate, &command_buffer));
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
#if !CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));
#endif

    VkImageMemoryBarrier cube_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
#if CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
#else
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
#endif
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = cube_image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                             CUBE_ARRAY_STORAGE_LAYERS},
    };
#if CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
    for (unsigned layer = 0; layer < CUBE_ARRAY_STORAGE_LAYERS; ++layer) {
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));
        uint8_t rgba[4];
        cube_array_expected_color(layer, rgba);
        VkClearValue face_clear = {.color = {.float32 = {
            rgba[0] / 255.0f, rgba[1] / 255.0f,
            rgba[2] / 255.0f, rgba[3] / 255.0f,
        }}};
        VkRenderPassBeginInfo face_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = render_pass,
            .framebuffer = face_framebuffers[layer],
            .renderArea = {{0, 0}, {CUBE_ARRAY_FACE_EXTENT, CUBE_ARRAY_FACE_EXTENT}},
            .clearValueCount = 1,
            .pClearValues = &face_clear,
        };
        vkCmdBeginRenderPass(command_buffer, &face_begin, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment face_attachment = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0,
            .clearValue = face_clear,
        };
        VkClearRect face_rect = {
            .rect = {{0, 0}, {CUBE_ARRAY_FACE_EXTENT, CUBE_ARRAY_FACE_EXTENT}},
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        vkCmdClearAttachments(command_buffer, 1, &face_attachment, 1, &face_rect);
        vkCmdEndRenderPass(command_buffer);
        CHECK(vkEndCommandBuffer(command_buffer));
        VkSubmitInfo face_submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command_buffer,
        };
        CHECK(vkQueueSubmit(queue, 1, &face_submit, fence));
        REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE,
                                UINT64_C(5000000000)) == VK_SUCCESS,
                "cube-array face clear completes within its bounded fence");
        CHECK(vkResetFences(device, 1, &fence));
        CHECK(vkResetCommandBuffer(command_buffer, 0));
    }
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &cube_barrier);
#else
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &cube_barrier);
    VkBufferImageCopy face_copies[CUBE_ARRAY_STORAGE_LAYERS];
    for (unsigned layer = 0; layer < CUBE_ARRAY_STORAGE_LAYERS; ++layer) {
        face_copies[layer] = (VkBufferImageCopy){
            .bufferOffset = layer * CUBE_ARRAY_FACE_BYTES,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {CUBE_ARRAY_FACE_EXTENT, CUBE_ARRAY_FACE_EXTENT, 1},
        };
    }
    vkCmdCopyBufferToImage(command_buffer, upload_buffer, cube_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           CUBE_ARRAY_STORAGE_LAYERS, face_copies);
    cube_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    cube_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    cube_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    cube_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &cube_barrier);
#endif

    VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo render_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = framebuffer,
        .renderArea = {{0, 0}, {CUBE_ARRAY_TARGET_WIDTH, CUBE_ARRAY_TARGET_HEIGHT}},
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(command_buffer);

    VkImageMemoryBarrier target_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target_image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &target_barrier);
    VkBufferImageCopy target_copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {CUBE_ARRAY_TARGET_WIDTH, CUBE_ARRAY_TARGET_HEIGHT, 1},
    };
    vkCmdCopyImageToBuffer(command_buffer, target_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_buffer, 1, &target_copy);
    VkBufferMemoryBarrier readback_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback_buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &readback_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    const VkResult wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE,
                                                  UINT64_C(5000000000));
    REQUIRE(wait_result == VK_SUCCESS,
            "cube-array witness GPU work completes within its bounded fence");
    VkMappedMemoryRange readback_invalidate = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = readback_memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &readback_invalidate));

    const uint8_t *pixels = readback_bytes;
    uint32_t mismatches = 0;
#if CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
    for (unsigned cell = 0; cell < CUBE_ARRAY_STORAGE_LAYERS; ++cell) {
        const uint8_t *actual = pixels + cell * CUBE_ARRAY_CELL_EXTENT * 4u;
        uint8_t expected[4];
        cube_array_expected_color(cell + CONSUMER_CUBE_ARRAY_BASE_LAYER, expected);
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_CONSUMER_CUBE_ARRAY_CELL cell=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u",
            cell, actual[0], actual[1], actual[2], actual[3],
            expected[0], expected[1], expected[2], expected[3]);
    }
#endif
    for (unsigned y = 0; y < CUBE_ARRAY_TARGET_HEIGHT; ++y) {
        for (unsigned x = 0; x < CUBE_ARRAY_TARGET_WIDTH; ++x) {
            const unsigned cell = x / CUBE_ARRAY_CELL_EXTENT;
            uint8_t expected[4];
            cube_array_expected_color(cell + CONSUMER_CUBE_ARRAY_BASE_LAYER, expected);
            const uint8_t *actual = pixels +
                (y * CUBE_ARRAY_TARGET_WIDTH + x) * CUBE_ARRAY_PIXEL_BYTES;
            mismatches += actual[0] != expected[0] || actual[1] != expected[1] ||
                          actual[2] != expected[2] || actual[3] != expected[3];
        }
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_CUBE_ARRAY_RESULT cells=%u pixels=%u mismatches=%u"
        " face_order=+x,-x,+y,-y,+z,-z cube_count=2",
        CUBE_ARRAY_LAYER_COUNT, CUBE_ARRAY_TARGET_WIDTH * CUBE_ARRAY_TARGET_HEIGHT,
        mismatches);
    REQUIRE(mismatches == 0,
            "all faces from both cubes produce their exact uploaded color on GPU");

    vkUnmapMemory(device, readback_memory);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyShaderModule(device, fragment_module, NULL);
    vkDestroyShaderModule(device, vertex_module, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
#if CONSUMER_CUBE_ARRAY_TILED_ATTACHMENT
    for (unsigned layer = 0; layer < CUBE_ARRAY_STORAGE_LAYERS; ++layer) {
        vkDestroyFramebuffer(device, face_framebuffers[layer], NULL);
        vkDestroyImageView(device, face_views[layer], NULL);
    }
#endif
    vkDestroyRenderPass(device, render_pass, NULL);
    vkDestroyImageView(device, target_view, NULL);
    vkDestroyImage(device, target_image, NULL);
    vkFreeMemory(device, target_memory, NULL);
    vkDestroyBuffer(device, readback_buffer, NULL);
    vkFreeMemory(device, readback_memory, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(device, descriptor_layout, NULL);
    vkDestroySampler(device, sampler, NULL);
    vkDestroyImageView(device, cube_view, NULL);
    vkDestroyImage(device, cube_image, NULL);
    vkFreeMemory(device, cube_memory, NULL);
    vkDestroyBuffer(device, upload_buffer, NULL);
    vkFreeMemory(device, upload_memory, NULL);
    ps5log_line(PS5LOG_MARK,
        "PS5VK_CONSUMER_CUBE_ARRAY_RETIRED fence_complete=1 allocations=0");
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public-SDK-only BC mip/layer transfer and sampling witness, one format per executable.
 * Included after CHECK/REQUIRE, ps5log and the generated shader arrays.
 */
#include "bc_subresource_shaders.h"
#include "bc_subresource_data.h"

enum {
    BC_SUBRESOURCE_STORAGE_LAYERS = 3,
    BC_SUBRESOURCE_TARGET_WIDTH = 64, BC_SUBRESOURCE_TARGET_HEIGHT = 64,
    BC_SUBRESOURCE_PIXEL_BYTES = 4,
};

static VkDeviceMemory bc_subresource_allocate(VkDevice device, VkDeviceSize size)
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

static void run_bc_subresource_witness(VkPhysicalDevice physical, VkDevice device,
                                   VkQueue queue)
{
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_BC_SUBRESOURCE_START profile=%s format=%u image=13x9 mips=4 layers=3 mip=%u layer=%u"
        " input_sha256=%s raw_reference_sha256=%s reference_sha256=%s",
        BC_SUBRESOURCE_PROFILE, (unsigned)BC_SUBRESOURCE_FORMAT, BC_SUBRESOURCE_MIP, BC_SUBRESOURCE_LAYER,
        BC_SUBRESOURCE_INPUT_SHA256, BC_SUBRESOURCE_RAW_REFERENCE_SHA256,
        BC_SUBRESOURCE_REFERENCE_SHA256);

    VkFormatProperties format_properties = {0};
    vkGetPhysicalDeviceFormatProperties(physical, BC_SUBRESOURCE_FORMAT,
                                        &format_properties);
    const VkFormatFeatureFlags sampled_required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    REQUIRE((format_properties.optimalTilingFeatures & sampled_required) == sampled_required,
            "BC subresource format reports sampled and both transfer directions");
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
    const VkFormatFeatureFlags target_required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    REQUIRE((format_properties.optimalTilingFeatures & target_required) == target_required,
            "BC subresource witness target reports attachment and transfer-source support");

    VkImage sampled_image = VK_NULL_HANDLE;
    VkDeviceMemory sampled_memory = VK_NULL_HANDLE;
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkImageCreateInfo sampled_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = BC_SUBRESOURCE_FORMAT,
        .extent = {13, 9, 1},
        .mipLevels = 4,
        .arrayLayers = BC_SUBRESOURCE_STORAGE_LAYERS,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(device, &sampled_info, NULL, &sampled_image));
    VkMemoryRequirements sampled_requirements = {0};
    vkGetImageMemoryRequirements(device, sampled_image, &sampled_requirements);
    sampled_memory = bc_subresource_allocate(device, sampled_requirements.size);
    CHECK(vkBindImageMemory(device, sampled_image, sampled_memory, 0));
    VkImageViewCreateInfo sampled_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = sampled_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = BC_SUBRESOURCE_FORMAT,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, BC_SUBRESOURCE_MIP, 1,
                             BC_SUBRESOURCE_LAYER, 1},
    };
    CHECK(vkCreateImageView(device, &sampled_view_info, NULL, &sampled_view));

    VkBuffer upload_buffer = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE;
    VkBufferCreateInfo upload_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(bc_subresource_blocks),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(device, &upload_info, NULL, &upload_buffer));
    VkMemoryRequirements upload_requirements = {0};
    vkGetBufferMemoryRequirements(device, upload_buffer, &upload_requirements);
    upload_memory = bc_subresource_allocate(device, upload_requirements.size);
    CHECK(vkBindBufferMemory(device, upload_buffer, upload_memory, 0));
    uint8_t *upload_bytes = NULL;
    CHECK(vkMapMemory(device, upload_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&upload_bytes));
    memcpy(upload_bytes, bc_subresource_blocks, sizeof(bc_subresource_blocks));
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
        .imageView = sampled_view,
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
        .extent = {BC_SUBRESOURCE_TARGET_WIDTH, BC_SUBRESOURCE_TARGET_HEIGHT, 1},
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
    target_memory = bc_subresource_allocate(device, target_requirements.size);
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
        .size = BC_SUBRESOURCE_TARGET_WIDTH * BC_SUBRESOURCE_TARGET_HEIGHT *
                BC_SUBRESOURCE_PIXEL_BYTES + sizeof(bc_subresource_raw_expected),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(device, &readback_info, NULL, &readback_buffer));
    VkMemoryRequirements readback_requirements = {0};
    vkGetBufferMemoryRequirements(device, readback_buffer, &readback_requirements);
    readback_memory = bc_subresource_allocate(device, readback_requirements.size);
    CHECK(vkBindBufferMemory(device, readback_buffer, readback_memory, 0));
    uint8_t *readback_bytes = NULL;
    CHECK(vkMapMemory(device, readback_memory, 0, readback_requirements.size, 0,
                      (void **)&readback_bytes));

    memset(readback_bytes, 0xa5, (size_t)readback_info.size);
    VkMappedMemoryRange readback_flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = readback_memory, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    CHECK(vkFlushMappedMemoryRanges(device, 1, &readback_flush));

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
        .width = BC_SUBRESOURCE_TARGET_WIDTH,
        .height = BC_SUBRESOURCE_TARGET_HEIGHT,
        .layers = 1,
    };
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(bc_subresource_vert_spirv),
        .pCode = bc_subresource_vert_spirv,
    };
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &vertex_module));
    shader_info.codeSize = sizeof(bc_subresource_frag_spirv);
    shader_info.pCode = bc_subresource_frag_spirv;
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
    VkViewport viewport = {0.0f, 0.0f, BC_SUBRESOURCE_TARGET_WIDTH,
                           BC_SUBRESOURCE_TARGET_HEIGHT, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {BC_SUBRESOURCE_TARGET_WIDTH, BC_SUBRESOURCE_TARGET_HEIGHT}};
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
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

    VkImageMemoryBarrier sampled_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sampled_image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, 0, 3},
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &sampled_barrier);
    vkCmdCopyBufferToImage(command_buffer, upload_buffer, sampled_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           12, bc_subresource_regions);
    sampled_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sampled_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sampled_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    sampled_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &sampled_barrier);

    /* Only the selected mip/layer becomes writable. Other subresources
     * remain readable; their exact bytes and all readback padding are checked. */
    sampled_barrier.subresourceRange = (VkImageSubresourceRange){
        VK_IMAGE_ASPECT_COLOR_BIT, BC_SUBRESOURCE_MIP, 1, BC_SUBRESOURCE_LAYER, 1};
    sampled_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sampled_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sampled_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sampled_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &sampled_barrier);
#if BC_SUBRESOURCE_IMAGE_COPY
    /* Distinct subresources of one image may have different layouts. The
     * source stays readable while only the destination layer is writable. */
    const VkImageCopy image_copy = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, BC_SUBRESOURCE_MIP, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, BC_SUBRESOURCE_MIP, BC_SUBRESOURCE_LAYER, 1},
        .dstOffset = {0, 0, 0},
        .extent = {6, 4, 1},
    };
    vkCmdCopyImage(command_buffer, sampled_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sampled_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy);
#else
    VkBufferImageCopy patch = bc_subresource_regions[BC_SUBRESOURCE_LAYER * 4 + BC_SUBRESOURCE_MIP];
    patch.bufferOffset = BC_SUBRESOURCE_PATCH_OFFSET;
    vkCmdCopyBufferToImage(command_buffer, upload_buffer, sampled_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &patch);
#endif

    /* Read an unselected mip while the selected one is still TRANSFER_DST:
     * an accidental whole-image layout change must fail this legal copy. */
    VkBufferImageCopy raw_copies[12];
    memcpy(raw_copies, bc_subresource_regions, sizeof(raw_copies));
    for (unsigned i = 0; i < 12; ++i)
        raw_copies[i].bufferOffset += BC_SUBRESOURCE_TARGET_WIDTH * BC_SUBRESOURCE_TARGET_HEIGHT * 4;
    vkCmdCopyImageToBuffer(command_buffer, sampled_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer,
                           1, &raw_copies[0]);
    sampled_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sampled_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sampled_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    sampled_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &sampled_barrier);
    vkCmdCopyImageToBuffer(command_buffer, sampled_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer,
                           11, raw_copies + 1);
    sampled_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sampled_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sampled_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sampled_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &sampled_barrier);

    VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo render_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = framebuffer,
        .renderArea = {{0, 0}, {BC_SUBRESOURCE_TARGET_WIDTH, BC_SUBRESOURCE_TARGET_HEIGHT}},
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
        .imageExtent = {BC_SUBRESOURCE_TARGET_WIDTH, BC_SUBRESOURCE_TARGET_HEIGHT, 1},
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
                                                  UINT64_C(300000000));
    REQUIRE(wait_result == VK_SUCCESS,
            "BC subresource witness GPU work completes within its bounded fence");
    VkMappedMemoryRange readback_invalidate = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = readback_memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &readback_invalidate));

    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_BC_SUBRESOURCE_FENCE complete=1 timeout_ns=300000000");
    const uint8_t *raw = readback_bytes + BC_SUBRESOURCE_TARGET_WIDTH * BC_SUBRESOURCE_TARGET_HEIGHT * 4;
    unsigned raw_mismatches = 0;
    for (unsigned i = 0; i < sizeof(bc_subresource_raw_expected); ++i) {
        if (raw[i] != bc_subresource_raw_expected[i] && raw_mismatches < 4)
            ps5log_printf(PS5LOG_INFO,
                "PS5VK_CONSUMER_BC_SUBRESOURCE_BYTE index=%u actual=%u expected=%u",
                i, raw[i], bc_subresource_raw_expected[i]);
        raw_mismatches += raw[i] != bc_subresource_raw_expected[i];
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_BC_SUBRESOURCE_RAW bytes=%u subresources=12 preserved=11 mismatches=%u",
        (unsigned)sizeof(bc_subresource_raw_expected), raw_mismatches);
    REQUIRE(raw_mismatches == 0, "all BC subresources and readback guards match exact reference bytes");

    const uint8_t *pixels = readback_bytes;
    unsigned mismatches = 0, max_error = 0;
    for (unsigned i = 0; i < BC_SUBRESOURCE_TARGET_WIDTH * BC_SUBRESOURCE_TARGET_HEIGHT; ++i) {
        unsigned bad = 0;
        for (unsigned c = 0; c < 4; ++c) {
            int delta = (int)pixels[i * 4 + c] - (int)bc_subresource_expected[i * 4 + c];
            unsigned error = (unsigned)(delta < 0 ? -delta : delta);
            if (error > max_error) max_error = error;
            bad |= error > BC_SUBRESOURCE_TOLERANCE;
        }
        if (bad && mismatches < 4)
            ps5log_printf(PS5LOG_INFO,
                "PS5VK_CONSUMER_BC_SUBRESOURCE_PIXEL index=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u",
                i, pixels[i*4], pixels[i*4+1], pixels[i*4+2], pixels[i*4+3],
                bc_subresource_expected[i*4], bc_subresource_expected[i*4+1],
                bc_subresource_expected[i*4+2], bc_subresource_expected[i*4+3]);
        mismatches += bad;
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_BC_SUBRESOURCE_RESULT pixels=4096 mismatches=%u max_error=%u tolerance=%u",
        mismatches, max_error, BC_SUBRESOURCE_TOLERANCE);
    REQUIRE(mismatches == 0, "GPU BC selected mip/layer samples match the independent CPU reference");

    vkUnmapMemory(device, readback_memory);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyShaderModule(device, fragment_module, NULL);
    vkDestroyShaderModule(device, vertex_module, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyRenderPass(device, render_pass, NULL);
    vkDestroyImageView(device, target_view, NULL);
    vkDestroyImage(device, target_image, NULL);
    vkFreeMemory(device, target_memory, NULL);
    vkDestroyBuffer(device, readback_buffer, NULL);
    vkFreeMemory(device, readback_memory, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(device, descriptor_layout, NULL);
    vkDestroySampler(device, sampler, NULL);
    vkDestroyImageView(device, sampled_view, NULL);
    vkDestroyImage(device, sampled_image, NULL);
    vkFreeMemory(device, sampled_memory, NULL);
    vkDestroyBuffer(device, upload_buffer, NULL);
    vkFreeMemory(device, upload_memory, NULL);
    ps5log_line(PS5LOG_MARK,
        "PS5VK_CONSUMER_BC_SUBRESOURCE_RETIRED fence_complete=1 allocations=0");
}

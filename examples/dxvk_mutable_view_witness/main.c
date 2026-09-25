/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mutable-format view witness: a public-SDK payload (no private headers or
 * symbols) for the format routes the first DXVK D3D11 workload uses.
 *
 * Reporting: VkFormatProperties3 (VK_KHR_format_feature_flags2) must equal
 * the 32-bit answer for RGBA8 UNORM and SRGB, and DXVK's exact render-target
 * query (R8G8B8A8_UNORM, 2D, OPTIMAL, TRANSFER_SRC|TRANSFER_DST|
 * COLOR_ATTACHMENT, MUTABLE_FORMAT, no format list) must succeed.
 *
 * Execution: the render target is DXVK's exact image - RGBA8 UNORM, usage
 * 0x13, MUTABLE_FORMAT with the VkImageFormatListCreateInfo {UNORM, SRGB} -
 * rendered through its UNORM view. A 16x16 texture whose R and G channels
 * each hold every byte value once is uploaded twice: into a MUTABLE RGBA8
 * UNORM image (list {UNORM, SRGB}) and into an ordinary RGBA8 SRGB image.
 * Three draws sample one texel per pixel (nearest, at texel centres) through
 *   0. the mutable image's UNORM view  -> must return the bytes unchanged;
 *   1. the mutable image's SRGB view   -> the reinterpretation under test;
 *   2. the SRGB image's own view       -> control: the witnessed SRGB path.
 * Draw 1 must match draw 2 byte for byte, stay within one code of the exact
 * sRGB decode (IEC 61966-2-1, rounded to nearest by the UNORM target) with
 * alpha unchanged, and differ from draw 0 wherever the decode changes a code.
 * An SRGB view of the render target inherits COLOR_ATTACHMENT, which SRGB does
 * not serve, so its creation must be refused. Every submission waits on a
 * bounded fence.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "dxvk_mutable_view_shaders.h"
#include "dxvk_mutable_view_data.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { EXTENT = 16, PIXELS = EXTENT * EXTENT, BYTES = PIXELS * 4, DRAWS = 3 };
static const uint64_t fence_timeout = UINT64_C(300000000);
static const VkFormat family[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
static const VkImageUsageFlags rt_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
static const VkImageUsageFlags texture_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static uint32_t digest_bytes(const uint8_t *p, size_t bytes)
{
    uint32_t digest = UINT32_C(2166136261);
    for (size_t i = 0; i < bytes; ++i) digest = (digest ^ p[i]) * UINT32_C(16777619);
    return digest;
}

struct image {
    VkImage image;
    VkDeviceMemory memory;
};

static VkResult make_image(VkDevice device, VkFormat format, VkImageUsageFlags usage,
                           VkBool32 mutable_format, struct image *out)
{
    VkImageFormatListCreateInfo list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = family};
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = mutable_format ? &list : NULL,
        .flags = mutable_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {EXTENT, EXTENT, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult rc = vkCreateImage(device, &info, NULL, &out->image);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, out->image, &req);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0};
    rc = vkAllocateMemory(device, &allocation, NULL, &out->memory);
    if (rc != VK_SUCCESS) return rc;
    return vkBindImageMemory(device, out->image, out->memory, 0);
}

static void destroy_image(VkDevice device, struct image *image)
{
    if (image->image) vkDestroyImage(device, image->image, NULL);
    if (image->memory) vkFreeMemory(device, image->memory, NULL);
    memset(image, 0, sizeof(*image));
}

static VkResult make_view(VkDevice device, VkImage image, VkFormat format, VkImageView *out)
{
    VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    return vkCreateImageView(device, &info, NULL, out);
}

static VkResult make_buffer(VkDevice device, VkDeviceSize bytes, VkBufferUsageFlags usage,
                            VkBuffer *buffer, VkDeviceMemory *memory, uint8_t **host)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult rc = vkCreateBuffer(device, &info, NULL, buffer);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, *buffer, &req);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0};
    rc = vkAllocateMemory(device, &allocation, NULL, memory);
    if (rc != VK_SUCCESS) return rc;
    rc = vkBindBufferMemory(device, *buffer, *memory, 0);
    if (rc != VK_SUCCESS) return rc;
    return vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, (void **)host);
}

static VkResult flush_or_invalidate(VkDevice device, VkDeviceMemory memory, int invalidate)
{
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .size = VK_WHOLE_SIZE};
    return invalidate ? vkInvalidateMappedMemoryRanges(device, 1, &range) :
                        vkFlushMappedMemoryRanges(device, 1, &range);
}

static void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
    VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(condition, reason) do { if (!(condition)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    struct image target = {0}, mutable_texture = {0}, srgb_texture = {0};
    VkImageView target_view = VK_NULL_HANDLE, refused_view = VK_NULL_HANDLE;
    VkImageView sampled_views[DRAWS] = {VK_NULL_HANDLE};
    VkBuffer upload = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE, readback_memory = VK_NULL_HANDLE;
    uint8_t *upload_bytes = NULL, *readback_bytes = NULL;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[DRAWS] = {VK_NULL_HANDLE};
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[1 + DRAWS] = {VK_NULL_HANDLE};
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 pending = VK_FALSE;

    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");

    /* Both routes must be enumerated before they are used. */
    VkExtensionProperties extensions[32];
    uint32_t extension_count = 32;
    TRY(vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions));
    uint32_t flags2_spec = 0, list_spec = 0;
    for (uint32_t n = 0; n < extension_count; ++n) {
        if (!strcmp(extensions[n].extensionName, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME))
            flags2_spec = extensions[n].specVersion;
        if (!strcmp(extensions[n].extensionName, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME))
            list_spec = extensions[n].specVersion;
    }

    /* VkFormatProperties3 is DXVK's only format-support answer. */
    VkFormatProperties3 p3[2];
    VkFormatProperties2 p2[2];
    for (unsigned f = 0; f < 2; ++f) {
        p3[f] = (VkFormatProperties3){.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        p2[f] = (VkFormatProperties2){.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                      .pNext = &p3[f]};
        vkGetPhysicalDeviceFormatProperties2KHR(physical, family[f], &p2[f]);
    }
    VkPhysicalDeviceImageFormatInfo2 query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = rt_usage,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
    VkImageFormatProperties2 limits = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    const VkResult rt_query = vkGetPhysicalDeviceImageFormatProperties2KHR(physical, &query,
                                                                          &limits);
    ps5log_printf(PS5LOG_MARK,
        "DXVK_MUTABLE_VIEW_WITNESS_START extent=%u flags2_spec=%u list_spec=%u"
        " unorm32=%08x unorm64=%016llx srgb32=%08x srgb64=%016llx rt_query=%d rt_max=%ux%u",
        EXTENT, flags2_spec, list_spec,
        (unsigned)p2[0].formatProperties.optimalTilingFeatures,
        (unsigned long long)p3[0].optimalTilingFeatures,
        (unsigned)p2[1].formatProperties.optimalTilingFeatures,
        (unsigned long long)p3[1].optimalTilingFeatures, (int)rt_query,
        limits.imageFormatProperties.maxExtent.width,
        limits.imageFormatProperties.maxExtent.height);
    REQUIRE(flags2_spec && list_spec, "format routes enumerated");
    for (unsigned f = 0; f < 2; ++f)
        REQUIRE(p3[f].optimalTilingFeatures == p2[f].formatProperties.optimalTilingFeatures &&
                p3[f].linearTilingFeatures == p2[f].formatProperties.linearTilingFeatures &&
                p3[f].bufferFeatures == p2[f].formatProperties.bufferFeatures,
                "VkFormatProperties3 equals VkFormatProperties");
    REQUIRE(rt_query == VK_SUCCESS && limits.imageFormatProperties.maxExtent.width >= EXTENT,
            "DXVK render-target MUTABLE query");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    const char *device_extensions[2] = {VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME,
                                        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = device_extensions};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");

    TRY(make_image(device, VK_FORMAT_R8G8B8A8_UNORM, rt_usage, VK_TRUE, &target));
    TRY(make_image(device, VK_FORMAT_R8G8B8A8_UNORM, texture_usage, VK_TRUE, &mutable_texture));
    TRY(make_image(device, VK_FORMAT_R8G8B8A8_SRGB, texture_usage, VK_FALSE, &srgb_texture));
    TRY(make_view(device, target.image, VK_FORMAT_R8G8B8A8_UNORM, &target_view));
    const VkResult refused = make_view(device, target.image, VK_FORMAT_R8G8B8A8_SRGB,
                                       &refused_view);
    ps5log_printf(PS5LOG_MARK, "DXVK_MUTABLE_VIEW_WITNESS_REFUSAL view=target_srgb result=%d",
                  (int)refused);
    REQUIRE(refused != VK_SUCCESS, "SRGB colour-attachment view refused");
    TRY(make_view(device, mutable_texture.image, VK_FORMAT_R8G8B8A8_UNORM, &sampled_views[0]));
    TRY(make_view(device, mutable_texture.image, VK_FORMAT_R8G8B8A8_SRGB, &sampled_views[1]));
    TRY(make_view(device, srgb_texture.image, VK_FORMAT_R8G8B8A8_SRGB, &sampled_views[2]));

    TRY(make_buffer(device, BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload,
                    &upload_memory, &upload_bytes));
    memcpy(upload_bytes, mutable_view_texels, BYTES);
    TRY(flush_or_invalidate(device, upload_memory, 0));
    TRY(make_buffer(device, (VkDeviceSize)DRAWS * BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    &readback, &readback_memory, &readback_bytes));
    memset(readback_bytes, 0xcd, (size_t)DRAWS * BYTES);
    TRY(flush_or_invalidate(device, readback_memory, 0));

    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    TRY(vkCreateSampler(device, &sampler_info, NULL, &sampler));
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    TRY(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DRAWS};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = DRAWS, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    TRY(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetLayout set_layouts[DRAWS] = {set_layout, set_layout, set_layout};
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = DRAWS, .pSetLayouts = set_layouts};
    TRY(vkAllocateDescriptorSets(device, &set_info, sets));
    for (unsigned n = 0; n < DRAWS; ++n) {
        VkDescriptorImageInfo image_info = {.sampler = sampler, .imageView = sampled_views[n],
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = sets[n], .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info};
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    }

    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1,
        .pSubpasses = &subpass};
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &target_view,
        .width = EXTENT, .height = EXTENT, .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));
    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(mutable_view_vert_spirv), .pCode = mutable_view_vert_spirv};
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &vertex));
    shader_info.codeSize = sizeof(mutable_view_frag_spirv);
    shader_info.pCode = mutable_view_frag_spirv;
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &fragment));
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0.0f, 0.0f, EXTENT, EXTENT, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {EXTENT, EXTENT}};
    VkPipelineViewportStateCreateInfo viewport_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly, .pViewportState = &viewport_info,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = layout, .renderPass = pass};
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));

    VkCommandPoolCreateInfo pool_create = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0};
    TRY(vkCreateCommandPool(device, &pool_create, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 + DRAWS};
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    /* Upload the same bytes into both textures. */
    TRY(vkBeginCommandBuffer(commands[0], &begin));
    const VkImage textures[2] = {mutable_texture.image, srgb_texture.image};
    for (unsigned n = 0; n < 2; ++n) {
        image_barrier(commands[0], textures[n], VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {EXTENT, EXTENT, 1}};
        vkCmdCopyBufferToImage(commands[0], upload, textures[n],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        image_barrier(commands[0], textures[n], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    TRY(vkEndCommandBuffer(commands[0]));
    /* One draw per view into DXVK's render target, each read back on its own. */
    for (unsigned n = 0; n < DRAWS; ++n) {
        VkCommandBuffer command = commands[1 + n];
        TRY(vkBeginCommandBuffer(command, &begin));
        VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}};
        VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass, .framebuffer = framebuffer,
            .renderArea = {{0, 0}, {EXTENT, EXTENT}}, .clearValueCount = 1,
            .pClearValues = &clear};
        vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                &sets[n], 0, NULL);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        image_barrier(command, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {.bufferOffset = (VkDeviceSize)n * BYTES,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {EXTENT, EXTENT, 1}};
        vkCmdCopyImageToBuffer(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback, 1, &region);
        VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = readback, .offset = (VkDeviceSize)n * BYTES, .size = BYTES};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, NULL, 1, &host, 0, NULL);
        TRY(vkEndCommandBuffer(command));
    }
    for (unsigned n = 0; n < 1 + DRAWS; ++n) {
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &commands[n]};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        pending = VK_TRUE;
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
        pending = VK_FALSE;
        TRY(vkResetFences(device, 1, &fence));
    }
    TRY(flush_or_invalidate(device, readback_memory, 1));

    /* Oracles. */
    const uint8_t *out[DRAWS] = {readback_bytes, readback_bytes + BYTES,
                                 readback_bytes + 2 * BYTES};
    uint32_t unorm_mismatches = 0, native_mismatches = 0, over_one = 0, exact = 0;
    uint32_t alpha_mismatches = 0, differing = 0;
    for (uint32_t i = 0; i < BYTES; ++i) {
        unorm_mismatches += out[0][i] != mutable_view_texels[i];
        native_mismatches += out[1][i] != out[2][i];
        const int error = (int)out[1][i] - (int)mutable_view_srgb_exact[i];
        over_one += error < -1 || error > 1;
        exact += !error;
        if ((i & 3u) == 3u) alpha_mismatches += out[1][i] != mutable_view_texels[i];
        else differing += out[1][i] != out[0][i];
    }
    const uint32_t digests[DRAWS] = {digest_bytes(out[0], BYTES), digest_bytes(out[1], BYTES),
                                     digest_bytes(out[2], BYTES)};
    ps5log_printf(PS5LOG_MARK,
        "DXVK_MUTABLE_VIEW_WITNESS_RESULT bytes=%u unorm_mismatches=%u srgb_vs_native=%u"
        " srgb_over_one=%u srgb_exact=%u alpha_mismatches=%u srgb_vs_unorm_differing=%u"
        " digest_unorm=%08x digest_srgb=%08x digest_native=%08x",
        BYTES, unorm_mismatches, native_mismatches, over_one, exact, alpha_mismatches,
        differing, digests[0], digests[1], digests[2]);
    REQUIRE(!unorm_mismatches, "UNORM view returns the bytes");
    REQUIRE(!native_mismatches, "SRGB view equals the SRGB image");
    REQUIRE(!over_one && !alpha_mismatches, "SRGB view within one code of the exact decode");
    REQUIRE(differing >= MUTABLE_VIEW_MIN_DIFFERING, "SRGB decode applied");

cleanup:
    if (pending && device && vkDeviceWaitIdle(device) != VK_SUCCESS) {
        ps5log_printf(PS5LOG_ERR,
            "DXVK_MUTABLE_VIEW_WITNESS_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "idle", (int)result);
        return 1;
    }
    if (fence) vkDestroyFence(device, fence, NULL);
    if (commands[0]) vkFreeCommandBuffers(device, pool, 1 + DRAWS, commands);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (fragment) vkDestroyShaderModule(device, fragment, NULL);
    if (vertex) vkDestroyShaderModule(device, vertex, NULL);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (sampler) vkDestroySampler(device, sampler, NULL);
    if (readback) vkDestroyBuffer(device, readback, NULL);
    if (readback_memory) vkFreeMemory(device, readback_memory, NULL);
    if (upload) vkDestroyBuffer(device, upload, NULL);
    if (upload_memory) vkFreeMemory(device, upload_memory, NULL);
    for (unsigned n = 0; n < DRAWS; ++n)
        if (sampled_views[n]) vkDestroyImageView(device, sampled_views[n], NULL);
    if (refused_view) vkDestroyImageView(device, refused_view, NULL);
    if (target_view) vkDestroyImageView(device, target_view, NULL);
    destroy_image(device, &srgb_texture);
    destroy_image(device, &mutable_texture);
    destroy_image(device, &target);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "DXVK_MUTABLE_VIEW_WITNESS_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR,
            "DXVK_MUTABLE_VIEW_WITNESS_FAILURE call=%s result=%d retirement=attempted",
            failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
#undef TRY
#undef REQUIRE
}

int main(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t boot = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
    ps5log_config config;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if (ps5log_load_config(paths, 1, &config, &loaded)) _exit(1);
    config.udp = 0;
    if (ps5log_init(&config, "PPSA99994", "ps5vk", boot)) _exit(1);
    int failed = run_witness();
    ps5log_close(failed ? "dxvk-mutable-view-witness-failed" : "dxvk-mutable-view-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

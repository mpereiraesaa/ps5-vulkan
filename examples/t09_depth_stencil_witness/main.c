/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T09 depth/stencil witness: a public-SDK payload (no private headers or
 * symbols) that renders into a combined D32_SFLOAT_S8_UINT attachment and
 * reads each aspect back on its own.
 *
 * One depth-only pass clears depth to 1.0 and stencil to 0xa5, then
 *   - draws a full-target quad whose depth is the plane
 *     z = (x + 64y) / 8192 (a distinct value at every pixel centre), and
 *   - draws eight stencil-only coverage patterns with the depth test off and
 *     the stencil op INVERT under write mask 1 << k (dynamic): the pixels
 *     where bit k of g(x, y) = ((x ^ y) & 63) | (((y >> 4) & 3) << 6) is set,
 *     as plain geometry (no fragment discard).
 * The GPU therefore leaves depth (x + 0.5 + 64(y + 0.5)) / 8192 and stencil
 * 0xa5 ^ g(x, y) in the two planes: values that depend on every coordinate
 * bit, so a misplaced texel in either plane fails the readback.
 *
 * The aspects are then moved independently (separateDepthStencilLayouts):
 *   1. DEPTH only: DEPTH_ATTACHMENT_OPTIMAL -> TRANSFER_SRC, copy depth;
 *   2. STENCIL only: STENCIL_ATTACHMENT_OPTIMAL -> TRANSFER_SRC, copy stencil;
 *   3. copy depth again with no barrier (the depth aspect is still
 *      TRANSFER_SRC after the stencil-only transition, and its bytes are
 *      unchanged);
 *   4. DEPTH only: TRANSFER_SRC -> DEPTH_ATTACHMENT_OPTIMAL;
 *   5. copy stencil again with no barrier (the stencil aspect is still
 *      TRANSFER_SRC after the depth-only transition, bytes unchanged).
 * The driver refuses a copy of an aspect that is not in TRANSFER_SRC, so steps
 * 3 and 5 completing is itself evidence that each transition left the other
 * aspect's layout alone; the four readbacks show the contents were preserved.
 * Every submission waits on a bounded fence.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t09_depth_stencil_shaders.h"


#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { EXTENT = 64, PIXELS = EXTENT * EXTENT, STENCIL_CLEAR = 0xa5, STENCIL_BITS = 8 };
static const float depth_clear = 1.0f;
static const uint64_t fence_timeout = UINT64_C(2000000000);

static uint32_t digest_bytes(uint32_t digest, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < bytes; ++i) digest = (digest ^ p[i]) * UINT32_C(16777619);
    return digest;
}

static float expected_depth(uint32_t x, uint32_t y)
{
    return ((float)x + 0.5f + 64.0f * ((float)y + 0.5f)) / 8192.0f;
}

static uint8_t expected_stencil(uint32_t x, uint32_t y)
{
    return (uint8_t)(STENCIL_CLEAR ^ ((x ^ y) & 63u) ^ (((y >> 4) & 3u) << 6));
}

/* Adjacent pixels differ by 1/8192 in depth; the interpolation error of the
 * plane is orders of magnitude below that, so this tolerance still tells
 * every texel from every other one. */
static uint32_t depth_mismatches(const float *depth)
{
    uint32_t bad = 0;
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x) {
            const float error = depth[y * EXTENT + x] - expected_depth(x, y);
            if (!(error > -1.0f / 65536.0f && error < 1.0f / 65536.0f)) ++bad;
        }
    return bad;
}

static uint32_t stencil_mismatches(const uint8_t *stencil)
{
    uint32_t bad = 0;
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x)
            if (stencil[y * EXTENT + x] != expected_stencil(x, y)) ++bad;
    return bad;
}

struct readback {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *host;
    VkDeviceSize bytes;
};

static VkResult make_readback(VkDevice device, VkDeviceSize bytes, struct readback *out)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult rc = vkCreateBuffer(device, &info, NULL, &out->buffer);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out->buffer, &req);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0,
    };
    rc = vkAllocateMemory(device, &allocation, NULL, &out->memory);
    if (rc != VK_SUCCESS) return rc;
    rc = vkBindBufferMemory(device, out->buffer, out->memory, 0);
    if (rc != VK_SUCCESS) return rc;
    rc = vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->host);
    if (rc != VK_SUCCESS) return rc;
    /* A sentinel the GPU readback must overwrite. */
    memset(out->host, 0xcd, (size_t)bytes);
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = out->memory, .size = VK_WHOLE_SIZE,
    };
    out->bytes = bytes;
    return vkFlushMappedMemoryRanges(device, 1, &range);
}

static void destroy_readback(VkDevice device, struct readback *r)
{
    if (r->host) vkUnmapMemory(device, r->memory);
    if (r->buffer) vkDestroyBuffer(device, r->buffer, NULL);
    if (r->memory) vkFreeMemory(device, r->memory, NULL);
    memset(r, 0, sizeof(*r));
}

static VkResult invalidate(VkDevice device, const struct readback *r)
{
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = r->memory, .size = VK_WHOLE_SIZE,
    };
    return vkInvalidateMappedMemoryRanges(device, 1, &range);
}

/* One aspect-only barrier. */
static void aspect_barrier(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect,
    VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access,
    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* Copy one aspect into its buffer and publish it to the host. */
static void copy_aspect(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect,
    const struct readback *r)
{
    VkBufferImageCopy region = {
        .imageSubresource = {aspect, 0, 0, 1},
        .imageExtent = {EXTENT, EXTENT, 1},
    };
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           r->buffer, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->buffer, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host, 0, NULL);
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
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkShaderModule cells_vertex = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE, plain_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE, stencil_pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[6] = {VK_NULL_HANDLE};
    VkFence fence = VK_NULL_HANDLE;
    struct readback depth_first = {0}, stencil_first = {0};
    struct readback depth_again = {0}, stencil_again = {0};
    unsigned completed = 0;
    uint32_t mismatches[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t depth_digest = 0, stencil_digest = 0;

    /* The Vulkan 1.0 route to separateDepthStencilLayouts, negotiated the way
     * an application must: properties2 on the instance, then the feature
     * query and the extension chain the pinned registry requires. */
    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");

    /* The format is chosen the way an application (DXVK) chooses it: through
     * the format properties. The combined format must be an attachment and a
     * readback source; D24S8 must not be claimed, it is not measured. */
    VkFormatProperties d32s8 = {0}, d24s8 = {0};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D32_SFLOAT_S8_UINT, &d32s8);
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D24_UNORM_S8_UINT, &d24s8);
    ps5log_printf(PS5LOG_MARK, "T09_DS_WITNESS_START extent=%u d32s8=%08x d24s8=%08x",
        EXTENT, (unsigned)d32s8.optimalTilingFeatures, (unsigned)d24s8.optimalTilingFeatures);
    REQUIRE((d32s8.optimalTilingFeatures & (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
             VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) ==
            (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT),
            "D32_SFLOAT_S8_UINT attachment and readback");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures separate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES};
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &separate};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features2);
    ps5log_printf(PS5LOG_MARK, "T09_DS_WITNESS_FEATURE separateDepthStencilLayouts=%u",
        (unsigned)separate.separateDepthStencilLayouts);
    REQUIRE(separate.separateDepthStencilLayouts, "separateDepthStencilLayouts reported");
    const char *device_extensions[4] = {
        VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
        VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &separate,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 4, .ppEnabledExtensionNames = device_extensions,
    };
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
        .extent = {EXTENT, EXTENT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    TRY(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements image_req;
    vkGetImageMemoryRequirements(device, image, &image_req);
    VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_req.size, .memoryTypeIndex = 0,
    };
    TRY(vkAllocateMemory(device, &image_allocation, NULL, &image_memory));
    TRY(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1},
    };
    TRY(vkCreateImageView(device, &view_info, NULL, &view));

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pDepthStencilAttachment = &reference,
    };
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass,
    };
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view,
        .width = EXTENT, .height = EXTENT, .layers = 1,
    };
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    VkShaderModuleCreateInfo vertex_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t09_quad_vert_spirv), .pCode = t09_quad_vert_spirv,
    };
    VkShaderModuleCreateInfo fragment_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t09_depth_only_frag_spirv), .pCode = t09_depth_only_frag_spirv,
    };
    VkShaderModuleCreateInfo cells_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t09_stencil_cells_vert_spirv), .pCode = t09_stencil_cells_vert_spirv,
    };
    TRY(vkCreateShaderModule(device, &vertex_info, NULL, &vertex));
    TRY(vkCreateShaderModule(device, &fragment_info, NULL, &fragment));
    TRY(vkCreateShaderModule(device, &cells_info, NULL, &cells_vertex));
    VkPushConstantRange bit_range = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &bit_range,
    };
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineLayoutCreateInfo plain_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    TRY(vkCreatePipelineLayout(device, &plain_layout_info, NULL, &plain_layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = {0.0f, 0.0f, (float)EXTENT, (float)EXTENT, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {EXTENT, EXTENT}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    /* The depth pipeline writes the plane and leaves stencil alone. */
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS, .maxDepthBounds = 1.0f,
    };
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_state,
        .layout = plain_layout, .renderPass = pass, .subpass = 0,
    };
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    /* The stencil pipeline: no depth test or write, stencil ALWAYS with
     * INVERT on pass for both faces, the write mask and reference dynamic.
     * Both faces carry the same state so winding cannot hide a defect. */
    const VkStencilOpState invert = {
        .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_INVERT,
        .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
        .compareMask = 0xff, .writeMask = 0, .reference = 0,
    };
    VkPipelineDepthStencilStateCreateInfo stencil_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE, .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS, .stencilTestEnable = VK_TRUE,
        .front = invert, .back = invert, .maxDepthBounds = 1.0f,
    };
    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dynamic_states,
    };
    stages[0].module = cells_vertex;
    pipeline_info.pDepthStencilState = &stencil_state;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = layout;
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                  &stencil_pipeline));

    TRY(make_readback(device, PIXELS * 4u, &depth_first));
    TRY(make_readback(device, PIXELS * 4u, &depth_again));
    TRY(make_readback(device, PIXELS, &stencil_first));
    TRY(make_readback(device, PIXELS, &stencil_again));

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0,
    };
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 6,
    };
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    /* 0: the pass. */
    TRY(vkBeginCommandBuffer(commands[0], &begin));
    VkClearValue clear = {.depthStencil = {depth_clear, STENCIL_CLEAR}};
    VkRenderPassBeginInfo pass_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
        .framebuffer = framebuffer, .renderArea = {{0, 0}, {EXTENT, EXTENT}},
        .clearValueCount = 1, .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(commands[0], &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(commands[0], 6, 1, 0, 0);
    vkCmdBindPipeline(commands[0], VK_PIPELINE_BIND_POINT_GRAPHICS, stencil_pipeline);
    vkCmdSetStencilReference(commands[0], VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    for (uint32_t k = 0; k < STENCIL_BITS; ++k) {
        /* Checkerboard cells for the x ^ y bits, row bands for the last two. */
        const uint32_t rects = k < 6 ? ((EXTENT >> k) * (EXTENT >> k)) / 2u :
            EXTENT / (2u << (k - 2u));
        vkCmdSetStencilWriteMask(commands[0], VK_STENCIL_FACE_FRONT_AND_BACK, 1u << k);
        vkCmdPushConstants(commands[0], layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(k), &k);
        vkCmdDraw(commands[0], 6u * rects, 1, 0, 0);
    }
    vkCmdEndRenderPass(commands[0]);
    TRY(vkEndCommandBuffer(commands[0]));
    /* 1: depth only to TRANSFER_SRC, copy depth. */
    TRY(vkBeginCommandBuffer(commands[1], &begin));
    aspect_barrier(commands[1], image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy_aspect(commands[1], image, VK_IMAGE_ASPECT_DEPTH_BIT, &depth_first);
    TRY(vkEndCommandBuffer(commands[1]));
    /* 2: stencil only to TRANSFER_SRC, copy stencil. */
    TRY(vkBeginCommandBuffer(commands[2], &begin));
    aspect_barrier(commands[2], image, VK_IMAGE_ASPECT_STENCIL_BIT,
        VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy_aspect(commands[2], image, VK_IMAGE_ASPECT_STENCIL_BIT, &stencil_first);
    TRY(vkEndCommandBuffer(commands[2]));
    /* 3: depth again, no barrier. */
    TRY(vkBeginCommandBuffer(commands[3], &begin));
    copy_aspect(commands[3], image, VK_IMAGE_ASPECT_DEPTH_BIT, &depth_again);
    TRY(vkEndCommandBuffer(commands[3]));
    /* 4: depth only back to its attachment layout. */
    TRY(vkBeginCommandBuffer(commands[4], &begin));
    aspect_barrier(commands[4], image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    TRY(vkEndCommandBuffer(commands[4]));
    /* 5: stencil again, no barrier. */
    TRY(vkBeginCommandBuffer(commands[5], &begin));
    copy_aspect(commands[5], image, VK_IMAGE_ASPECT_STENCIL_BIT, &stencil_again);
    TRY(vkEndCommandBuffer(commands[5]));

    for (unsigned i = 0; i < 6; ++i) {
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &commands[i],
        };
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
        TRY(vkResetFences(device, 1, &fence));
        ++completed;
        ps5log_printf(PS5LOG_MARK, "T09_DS_WITNESS_STEP index=%u fence=complete", i);
    }
    TRY(invalidate(device, &depth_first));
    TRY(invalidate(device, &stencil_first));
    TRY(invalidate(device, &depth_again));
    TRY(invalidate(device, &stencil_again));
    mismatches[0] = depth_mismatches(depth_first.host);
    mismatches[1] = stencil_mismatches(stencil_first.host);
    mismatches[2] = depth_mismatches(depth_again.host);
    mismatches[3] = stencil_mismatches(stencil_again.host);
    depth_digest = digest_bytes(UINT32_C(2166136261), depth_first.host, PIXELS * 4u);
    stencil_digest = digest_bytes(UINT32_C(2166136261), stencil_first.host, PIXELS);
    {
        const uint32_t *d = depth_first.host;
        const uint8_t *st = stencil_first.host;
        ps5log_printf(PS5LOG_MARK,
            "T09_DS_WITNESS_SAMPLES depth_0_0=%08x depth_63_0=%08x depth_0_63=%08x depth_63_63=%08x "
            "stencil_0_0=%02x stencil_63_0=%02x stencil_0_63=%02x stencil_63_63=%02x",
            d[0], d[EXTENT - 1], d[(EXTENT - 1) * EXTENT], d[PIXELS - 1],
            st[0], st[EXTENT - 1], st[(EXTENT - 1) * EXTENT], st[PIXELS - 1]);
    }
    ps5log_printf(PS5LOG_MARK,
        "T09_DS_WITNESS_RESULT extent=%u depth_mismatches=%u stencil_mismatches=%u "
        "depth_after_stencil_mismatches=%u stencil_after_depth_mismatches=%u "
        "depth_digest=%08x stencil_digest=%08x submissions=%u fence=complete",
        EXTENT, mismatches[0], mismatches[1], mismatches[2], mismatches[3],
        depth_digest, stencil_digest, completed);

cleanup:
    if (failed)
        ps5log_printf(PS5LOG_ERR, "T09_DS_WITNESS_FAILURE result=%d step=%s completed=%u",
            (int)result, failed, completed);
    if (device) {
        vkDeviceWaitIdle(device);
        destroy_readback(device, &depth_first);
        destroy_readback(device, &stencil_first);
        destroy_readback(device, &depth_again);
        destroy_readback(device, &stencil_again);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
        if (stencil_pipeline) vkDestroyPipeline(device, stencil_pipeline, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (plain_layout) vkDestroyPipelineLayout(device, plain_layout, NULL);
        if (vertex) vkDestroyShaderModule(device, vertex, NULL);
        if (fragment) vkDestroyShaderModule(device, fragment, NULL);
        if (cells_vertex) vkDestroyShaderModule(device, cells_vertex, NULL);
        if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
        if (pass) vkDestroyRenderPass(device, pass, NULL);
        if (view) vkDestroyImageView(device, view, NULL);
        if (image) vkDestroyImage(device, image, NULL);
        if (image_memory) vkFreeMemory(device, image_memory, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    if (!failed)
        ps5log_printf(PS5LOG_MARK, "T09_DS_WITNESS_RETIRED resources=clean");
    return failed ? 1 : 0;
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
    ps5log_close(failed ? "t09-depth-stencil-witness-failed" : "t09-depth-stencil-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

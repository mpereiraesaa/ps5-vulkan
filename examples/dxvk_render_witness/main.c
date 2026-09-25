/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DXVK262-T10 render witness: a public-SDK payload (no private headers or
 * symbols) that replays the command shapes the pinned DXVK 2.6.2 records
 * for its first D3D11 frame (ClearRenderTargetView + Draw + CopyResource to
 * a STAGING texture + Map), measured against a host driver, through the
 * Vulkan 1.0 extension routes this device offers for them:
 *
 *   VK_KHR_dynamic_rendering    vkCmdBeginRenderingKHR with one colour
 *                               attachment, loadOp CLEAR, storeOp STORE, and a
 *                               pipeline created with renderPass = NULL and
 *                               VkPipelineRenderingCreateInfo; every stage
 *                               also chains its VkShaderModuleCreateInfo, as
 *                               DXVK does;
 *   VK_EXT_extended_dynamic_state  VIEWPORT/SCISSOR_WITH_COUNT, CULL_MODE and
 *                               FRONT_FACE dynamic, set before the draws;
 *   VK_KHR_maintenance1         DXVK's y-flipped viewport {0, 64, 64, -64};
 *   VK_KHR_copy_commands2       vkCmdCopyImageToBuffer2KHR into one staging
 *                               buffer at a nonzero offset, plus a second
 *                               region in the same command buffer;
 * with DXVK's barrier shape: a global dependency from the attachment writes,
 * the hand-over to TRANSFER_SRC, the copies, then one barrier call carrying
 * the global TRANSFER_WRITE -> HOST_READ publication and the hand-back. Only
 * the image barriers' access masks differ from DXVK's (which name no source
 * access): the device's Vulkan 1.0 barrier profile admits these.
 *
 * The pass clears to red, draws a gradient quad (R = 4x, G = 4y, B = 64)
 * over columns 0..47 with culling off, then two solid marker quads over
 * columns 48..63 - green over NDC y [-1, 0], blue over NDC y [0, 1], wound
 * opposite ways - with CULL_MODE = BACK and FRONT_FACE = CLOCKWISE. The
 * expected image is computed here from the Vulkan rules alone (the viewport
 * transform and the signed-area facing rule): the flip decides which rows each
 * marker lands on and which marker survives the cull, so a driver that
 * ignored the negative height, the cull mode or the front face fails the
 * exact comparison. Every byte of both regions and the sentinel bytes around
 * them are checked. One submission, one bounded fence.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "dxvk_render_witness_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { EXTENT = 64, STAGING = 64 * 1024 };
/* DXVK's staging slice sits at an offset inside a shared buffer. */
enum { FULL_OFFSET = 4096, SUB_OFFSET = 24576 + 256, SUB_X = 40, SUB_Y = 24,
       SUB_W = 24, SUB_H = 16, SUB_ROW = 32 };
static const uint64_t fence_timeout = UINT64_C(300000000);

static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{ return r | (g << 8) | (b << 16) | (a << 24); }

/* The quad corners the vertex shader emits, in NDC. */
static void corner(uint32_t index, float *x, float *y)
{
    uint32_t q = index / 6u, k = index % 6u;
    if (q == 2u) k = (k / 3u) * 3u + 2u - k % 3u;
    const float cx = (k == 1u || k == 4u || k == 5u) ? 1.0f : 0.0f;
    const float cy = (k == 2u || k == 3u || k == 5u) ? 1.0f : 0.0f;
    const float lo_x = q == 0u ? -1.0f : 0.5f, hi_x = q == 0u ? 0.5f : 1.0f;
    const float lo_y = q == 2u ? 0.0f : -1.0f, hi_y = q == 1u ? 0.0f : 1.0f;
    *x = lo_x + (hi_x - lo_x) * cx;
    *y = lo_y + (hi_y - lo_y) * cy;
}
/* Vulkan's facing rule for one triangle after the viewport transform
 * xf = ox + px/2 * xn, yf = oy + py/2 * yn: a = -1/2 sum(xf_i yf_i+1 - xf_i+1 yf_i),
 * front-facing when a < 0 for FRONT_FACE_CLOCKWISE. */
static int front_facing_clockwise(uint32_t first, float vx, float vy, float vw, float vh)
{
    float xf[3], yf[3];
    for (uint32_t i = 0; i < 3; ++i) {
        float xn, yn;
        corner(first + i, &xn, &yn);
        xf[i] = vx + vw * 0.5f + vw * 0.5f * xn;
        yf[i] = vy + vh * 0.5f + vh * 0.5f * yn;
    }
    float a = 0.0f;
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t j = (i + 1u) % 3u;
        a += xf[i] * yf[j] - xf[j] * yf[i];
    }
    return -0.5f * a < 0.0f;
}
/* The expected image: the clear, the gradient quad, and each marker quad the
 * back-face cull keeps, placed by the flipped viewport's row mapping. */
static void expected_image(uint32_t *image, float vx, float vy, float vw, float vh,
    uint32_t *visible_marker, uint32_t *marker_top)
{
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x)
            image[y * EXTENT + x] = x < 48u ? rgba(4u * x, 4u * y, 64u, 255u) :
                rgba(255u, 0u, 0u, 255u);
    *visible_marker = 0; *marker_top = UINT32_MAX;
    for (uint32_t q = 1; q <= 2; ++q) {
        /* Both triangles of a quad share its winding. */
        if (!front_facing_clockwise(q * 6u, vx, vy, vw, vh)) continue;
        float y0n = q == 1u ? -1.0f : 0.0f, y1n = q == 1u ? 0.0f : 1.0f;
        float y0 = vy + vh * 0.5f + vh * 0.5f * y0n, y1 = vy + vh * 0.5f + vh * 0.5f * y1n;
        const uint32_t top = (uint32_t)(y0 < y1 ? y0 : y1), bottom = (uint32_t)(y0 < y1 ? y1 : y0);
        const uint32_t color = q == 1u ? rgba(0u, 255u, 0u, 255u) : rgba(0u, 0u, 255u, 255u);
        for (uint32_t y = top; y < bottom; ++y)
            for (uint32_t x = 48; x < EXTENT; ++x) image[y * EXTENT + x] = color;
        *visible_marker |= 1u << q;
        if (top < *marker_top) *marker_top = top;
    }
}

static uint32_t digest_bytes(uint32_t digest, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < bytes; ++i) digest = (digest ^ p[i]) * UINT32_C(16777619);
    return digest;
}

#define PROC(device, name) ((PFN_##name)vkGetDeviceProcAddr((device), #name))

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
    VkDeviceMemory image_memory = VK_NULL_HANDLE, staging_memory = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    unsigned char *host = NULL;
    uint32_t full_mismatches = UINT32_MAX, sub_mismatches = UINT32_MAX,
        sentinel_mismatches = UINT32_MAX, visible = 0, marker_top = 0, digest = 0;
    static uint32_t expected[EXTENT * EXTENT];

    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");

    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .pNext = &eds};
    VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &dynamic_rendering};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features2);
    ps5log_printf(PS5LOG_MARK,
        "DXVK_RENDER_WITNESS_START extent=%u dynamicRendering=%u extendedDynamicState=%u",
        EXTENT, (unsigned)dynamic_rendering.dynamicRendering, (unsigned)eds.extendedDynamicState);
    REQUIRE(dynamic_rendering.dynamicRendering && eds.extendedDynamicState, "features reported");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    const char *device_extensions[] = {
        VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_1_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &dynamic_rendering, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = sizeof(device_extensions) / sizeof(device_extensions[0]),
        .ppEnabledExtensionNames = device_extensions};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    PFN_vkCmdBeginRenderingKHR begin_rendering = PROC(device, vkCmdBeginRenderingKHR);
    PFN_vkCmdEndRenderingKHR end_rendering = PROC(device, vkCmdEndRenderingKHR);
    PFN_vkCmdSetViewportWithCountEXT set_viewports = PROC(device, vkCmdSetViewportWithCountEXT);
    PFN_vkCmdSetScissorWithCountEXT set_scissors = PROC(device, vkCmdSetScissorWithCountEXT);
    PFN_vkCmdSetCullModeEXT set_cull = PROC(device, vkCmdSetCullModeEXT);
    PFN_vkCmdSetFrontFaceEXT set_front = PROC(device, vkCmdSetFrontFaceEXT);
    PFN_vkCmdCopyImageToBuffer2KHR copy_to_buffer = PROC(device, vkCmdCopyImageToBuffer2KHR);
    REQUIRE(begin_rendering && end_rendering && set_viewports && set_scissors && set_cull &&
            set_front && copy_to_buffer, "extension entry points resolve");

    /* DXVK's render target: TRANSFER_SRC | TRANSFER_DST | COLOR_ATTACHMENT. */
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {EXTENT, EXTENT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    TRY(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &allocation, NULL, &image_memory));
    TRY(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    TRY(vkCreateImageView(device, &view_info, NULL, &view));

    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = STAGING, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device, &buffer_info, NULL, &staging));
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    allocation.allocationSize = requirements.size;
    TRY(vkAllocateMemory(device, &allocation, NULL, &staging_memory));
    TRY(vkBindBufferMemory(device, staging, staging_memory, 0));
    TRY(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void **)&host));
    memset(host, 0xcd, STAGING);
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = staging_memory, .size = VK_WHOLE_SIZE};
    TRY(vkFlushMappedMemoryRanges(device, 1, &range));

    VkShaderModuleCreateInfo module_info[2] = {
        {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(dxvk_render_witness_vert_spirv), .pCode = dxvk_render_witness_vert_spirv},
        {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(dxvk_render_witness_frag_spirv), .pCode = dxvk_render_witness_frag_spirv}};
    TRY(vkCreateShaderModule(device, &module_info[0], NULL, &vertex));
    TRY(vkCreateShaderModule(device, &module_info[1], NULL, &fragment));
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    /* DXVK's monolithic pipeline shape (dxvk_graphics.cpp:1388-1437). */
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &module_info[0],
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &module_info[1],
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    const VkSampleMask sample_mask = 0xffffffffu;
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .pSampleMask = &sample_mask};
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xfu};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    const VkDynamicState dynamic_states[4] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT, VK_DYNAMIC_STATE_CULL_MODE_EXT,
        VK_DYNAMIC_STATE_FRONT_FACE_EXT};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 4, .pDynamicStates = dynamic_states};
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering_info,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_state, .pColorBlendState = &blend,
        .pDynamicState = &dynamic, .layout = layout, .basePipelineIndex = -1};
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));

    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(command, &begin));

    /* The attachment's initial transition (DXVK: UNDEFINED -> colour, the
     * clear covers every texel). */
    VkImageMemoryBarrier to_color = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &to_color);
    /* The folded ClearRenderTargetView: loadOp CLEAR, storeOp STORE. */
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {1.0f, 0.0f, 0.0f, 1.0f}}}};
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {EXTENT, EXTENT}}, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    begin_rendering(command, &rendering);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    /* DXVK's D3D viewport: y = H, height = -H (VK_KHR_maintenance1). */
    const VkViewport viewport = {0.0f, (float)EXTENT, (float)EXTENT, -(float)EXTENT, 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {EXTENT, EXTENT}};
    set_viewports(command, 1, &viewport);
    set_scissors(command, 1, &scissor);
    set_front(command, VK_FRONT_FACE_CLOCKWISE);
    set_cull(command, VK_CULL_MODE_NONE);
    vkCmdDraw(command, 6, 1, 0, 0);
    set_cull(command, VK_CULL_MODE_BACK_BIT);
    vkCmdDraw(command, 6, 1, 6, 0);
    vkCmdDraw(command, 6, 1, 12, 0);
    end_rendering(command);

    /* DXVK's readback: the global dependency from the attachment writes, the
     * hand-over with no source access, two regions, then one barrier call with
     * the host publication and the hand-back. */
    VkMemoryBarrier attachment_writes = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &attachment_writes, 0, NULL, 0, NULL);
    VkImageMemoryBarrier handover = to_color;
    /* DXVK's own hand-over names no source access (the global dependency
     * above made the writes available); the Vulkan 1.0 barrier profile of this
     * device admits the hand-over with the attachment write named, which
     * orders the same writes. */
    handover.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    handover.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    handover.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    handover.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &handover);
    VkBufferImageCopy2 full = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = FULL_OFFSET, .bufferRowLength = EXTENT, .bufferImageHeight = EXTENT,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {EXTENT, EXTENT, 1}};
    VkCopyImageToBufferInfo2 full_info = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .srcImage = image, .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstBuffer = staging, .regionCount = 1, .pRegions = &full};
    copy_to_buffer(command, &full_info);
    VkBufferImageCopy2 sub = full;
    sub.bufferOffset = SUB_OFFSET; sub.bufferRowLength = SUB_ROW; sub.bufferImageHeight = 0;
    sub.imageOffset = (VkOffset3D){SUB_X, SUB_Y, 0};
    sub.imageExtent = (VkExtent3D){SUB_W, SUB_H, 1};
    full_info.pRegions = &sub;
    copy_to_buffer(command, &full_info);
    VkMemoryBarrier publish = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT};
    VkImageMemoryBarrier handback = handover;
    handback.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    handback.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    handback.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    handback.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &publish, 0, NULL,
        1, &handback);
    TRY(vkEndCommandBuffer(command));

    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    TRY(vkQueueSubmit(queue, 1, &submit, fence));
    TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
    ps5log_printf(PS5LOG_MARK, "DXVK_RENDER_WITNESS_STEP index=0 fence=complete");
    TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));

    expected_image(expected, viewport.x, viewport.y, viewport.width, viewport.height,
        &visible, &marker_top);
    full_mismatches = sub_mismatches = sentinel_mismatches = 0;
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x) {
            uint32_t value;
            memcpy(&value, host + FULL_OFFSET + (y * EXTENT + x) * 4u, 4);
            full_mismatches += value != expected[y * EXTENT + x];
        }
    for (uint32_t y = 0; y < SUB_H; ++y)
        for (uint32_t x = 0; x < SUB_ROW; ++x) {
            uint32_t value;
            memcpy(&value, host + SUB_OFFSET + (y * SUB_ROW + x) * 4u, 4);
            if (x < SUB_W) sub_mismatches += value != expected[(SUB_Y + y) * EXTENT + SUB_X + x];
            else if (y + 1u < SUB_H) sentinel_mismatches += value != 0xcdcdcdcdu;
        }
    for (uint32_t b = 0; b < FULL_OFFSET; ++b) sentinel_mismatches += host[b] != 0xcd;
    for (uint32_t b = FULL_OFFSET + EXTENT * EXTENT * 4u; b < SUB_OFFSET; ++b)
        sentinel_mismatches += host[b] != 0xcd;
    for (uint32_t b = SUB_OFFSET + ((SUB_H - 1u) * SUB_ROW + SUB_W) * 4u; b < STAGING; ++b)
        sentinel_mismatches += host[b] != 0xcd;
    digest = digest_bytes(UINT32_C(2166136261), host + FULL_OFFSET, EXTENT * EXTENT * 4u);
    {
        uint32_t samples[5];
        const uint32_t at[5][2] = {{0, 0}, {47, 63}, {48, 0}, {63, 40}, {63, 63}};
        for (unsigned s = 0; s < 5; ++s)
            memcpy(&samples[s], host + FULL_OFFSET + (at[s][1] * EXTENT + at[s][0]) * 4u, 4);
        ps5log_printf(PS5LOG_MARK,
            "DXVK_RENDER_WITNESS_SAMPLES p0_0=%08x p47_63=%08x p48_0=%08x p63_40=%08x p63_63=%08x",
            samples[0], samples[1], samples[2], samples[3], samples[4]);
    }
    ps5log_printf(PS5LOG_MARK,
        "DXVK_RENDER_WITNESS_RESULT extent=%u full_mismatches=%u sub_mismatches=%u "
        "sentinel_mismatches=%u visible_markers=%x marker_top=%u digest=%08x "
        "submissions=1 fence=complete", EXTENT, full_mismatches, sub_mismatches,
        sentinel_mismatches, visible, marker_top, digest);

cleanup:
    if (failed)
        ps5log_printf(PS5LOG_ERR, "DXVK_RENDER_WITNESS_FAILURE result=%d step=%s",
            (int)result, failed);
    if (device) {
        vkDeviceWaitIdle(device);
        if (host) vkUnmapMemory(device, staging_memory);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (vertex) vkDestroyShaderModule(device, vertex, NULL);
        if (fragment) vkDestroyShaderModule(device, fragment, NULL);
        if (staging) vkDestroyBuffer(device, staging, NULL);
        if (staging_memory) vkFreeMemory(device, staging_memory, NULL);
        if (view) vkDestroyImageView(device, view, NULL);
        if (image) vkDestroyImage(device, image, NULL);
        if (image_memory) vkFreeMemory(device, image_memory, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    if (!failed) ps5log_printf(PS5LOG_MARK, "DXVK_RENDER_WITNESS_RETIRED resources=clean");
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
    ps5log_close(failed ? "dxvk-render-witness-failed" : "dxvk-render-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

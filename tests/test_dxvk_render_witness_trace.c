/* Host trace of the DXVK262-T10 render witness command buffer.
 *
 * examples/dxvk_render_witness/main.c records DXVK's first-frame shapes:
 * the UNDEFINED -> colour transition, a vkCmdBeginRenderingKHR instance with
 * the flipped viewport and the extended dynamic state, three draws, then
 * DXVK's readback (global dependency, hand-over with no source access, two
 * vkCmdCopyImageToBuffer2KHR regions, one barrier call with the host
 * publication and the hand-back). This fixture records the same calls through
 * the public entry points on the host object model, so every one of them is
 * accepted here before a console window is spent, and hands the recorded
 * postlude to the native region readback planner exactly as the graphics
 * queue does after the pass. The planned regions are then detiled from a
 * synthetic 64KB_R_X surface into the mapped staging buffer and compared byte
 * for byte, sentinels included. Only platform discovery and the compiler are
 * mocked; no GPU work runs. */
#include "vk_internal.h"
#include "vk_command.h"
#include "graphics_formats.h"
#include "graphics_program.h"
#include "physical_device_profile.h"
#include "readback_commands_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = *backing = calloc(1, (size_t)size);
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult sync_call(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, sync_call, sync_call};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    /* The witness build's platform bits (the shipping routes). */
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = 1u << 22, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features = PS5VK_FEATURE_MULTIVIEW,
        .supported_features_t09 = PS5VK_T09_FEATURE_MAINTENANCE2 |
            PS5VK_T09_FEATURE_CREATE_RENDERPASS2 | PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE |
            PS5VK_T09_FEATURE_COPY_COMMANDS2 | PS5VK_T09_FEATURE_DEPTH_STENCIL_RESOLVE |
            PS5VK_T09_FEATURE_DYNAMIC_RENDERING | PS5VK_T09_FEATURE_MAINTENANCE1};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 1u << 22,
        .allocation_granularity = 1, .buffer_image_granularity = 1};
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}
static unsigned compiled;
static VkResult compile_program(void *context, const struct ps5vk_graphics_key *key, const void **out)
{ (void)context; (void)key; *out = &compiled; return VK_SUCCESS; }
static void release_program(void *context, const void *data) { (void)context; (void)data; }
static VkResult create_graphics(VkDevice d, const void *data, uint32_t primitive, void **out)
{ (void)d; (void)data; (void)primitive; *out = malloc(1); return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release_graphics(VkDevice d, void *state) { (void)d; free(state); }

enum { EXTENT = 64, STAGING = 64 * 1024, FULL_OFFSET = 4096, SUB_OFFSET = 24576 + 256,
       SUB_X = 40, SUB_Y = 24, SUB_W = 24, SUB_H = 16, SUB_ROW = 32 };
static const uint32_t vs[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 0, 1, 0x6e69616d, 0};
static const uint32_t fs[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 4, 1, 0x6e69616d, 0};

int main(void)
{
    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    VkInstance instance = VK_NULL_HANDLE;
    assert(vkCreateInstance(&instance_info, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
        .extendedDynamicState = VK_TRUE};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .pNext = &eds,
        .dynamicRendering = VK_TRUE};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    const char *device_extensions[] = {
        VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_1_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &dynamic_rendering, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 8, .ppEnabledExtensionNames = device_extensions};
    VkDevice device = VK_NULL_HANDLE;
    assert(vkCreateDevice(physical, &device_info, NULL, &device) == VK_SUCCESS);
    device->graphics_enabled = VK_TRUE;
    device->image_requirements = ps5vk_native_image_requirements;
    device->graphics_compiler_context = &compiled;
    device->graphics_acquire = compile_program;
    device->graphics_compiled_release = release_program;
    device->graphics_create = create_graphics;
    device->graphics_release = release_graphics;

    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {EXTENT, EXTENT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImage image = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size};
    VkDeviceMemory image_memory = VK_NULL_HANDLE, staging_memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &image_memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view = VK_NULL_HANDLE;
    assert(vkCreateImageView(device, &view_info, NULL, &view) == VK_SUCCESS);
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = STAGING, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer staging = VK_NULL_HANDLE;
    assert(vkCreateBuffer(device, &buffer_info, NULL, &staging) == VK_SUCCESS);
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    allocation.allocationSize = requirements.size;
    assert(vkAllocateMemory(device, &allocation, NULL, &staging_memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, staging, staging_memory, 0) == VK_SUCCESS);

    VkShaderModuleCreateInfo module_info[2] = {
        {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vs), .pCode = vs},
        {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(fs), .pCode = fs}};
    VkShaderModule modules[2];
    for (unsigned n = 0; n < 2; ++n)
        assert(vkCreateShaderModule(device, &module_info[n], NULL, &modules[n]) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    assert(vkCreatePipelineLayout(device, &layout_info, NULL, &layout) == VK_SUCCESS);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &module_info[0],
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &module_info[1],
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"}};
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
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS};
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
    VkPipeline pipeline = VK_NULL_HANDLE;
    assert(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) ==
           VK_SUCCESS);

    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool = VK_NULL_HANDLE;
    assert(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &command_info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    /* DXVK's command-buffer split (host trace): InitBarriers takes the new
     * image UNDEFINED -> TRANSFER_DST; InitBuffer clears it to zero and hands
     * it over TRANSFER_DST -> COLOR_ATTACHMENT (stages 0x1000 -> 0x1400,
     * access 0x1000 -> 0x1980); Exec renders. All three go in one submit. */
    VkCommandBuffer init_barriers = VK_NULL_HANDLE, init_buffer = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo init_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        assert(VK_SUCCESS == vkAllocateCommandBuffers(device, &init_info, &init_barriers));
        assert(VK_SUCCESS == vkAllocateCommandBuffers(device, &init_info, &init_buffer));
        const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier to_transfer = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = all};
        assert(VK_SUCCESS == vkBeginCommandBuffer(init_barriers, &begin));
        vkCmdPipelineBarrier(init_barriers, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_transfer);
        assert(VK_SUCCESS == vkEndCommandBuffer(init_barriers));
        assert(VK_SUCCESS == vkBeginCommandBuffer(init_buffer, &begin));
        const VkClearColorValue zero = {{0.0f, 0.0f, 0.0f, 0.0f}};
        vkCmdClearColorImage(init_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
        VkImageMemoryBarrier to_attachment = to_transfer;
        to_attachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_attachment.dstAccessMask = 0x1980u;
        to_attachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(init_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, 0x1400u,
            0, 0, NULL, 0, NULL, 1, &to_attachment);
        assert(VK_SUCCESS == vkEndCommandBuffer(init_buffer));
    }
    assert(init_barriers->state == PS5VK_EXECUTABLE && init_buffer->state == PS5VK_EXECUTABLE);
    assert(init_buffer->operations[init_buffer->operation_count - 1].type == PS5VK_IMAGE_BARRIER);
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);

    /* The witness's calls, in its order (examples/dxvk_render_witness). */
    VkImageMemoryBarrier to_color = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &to_color);
    assert(command->state == PS5VK_RECORDING);
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {1.0f, 0.0f, 0.0f, 1.0f}}}};
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {EXTENT, EXTENT}}, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    vkCmdBeginRenderingKHR(command, &rendering);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkViewport viewport = {0.0f, (float)EXTENT, (float)EXTENT, -(float)EXTENT, 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {EXTENT, EXTENT}};
    vkCmdSetViewportWithCountEXT(command, 1, &viewport);
    vkCmdSetScissorWithCountEXT(command, 1, &scissor);
    vkCmdSetFrontFaceEXT(command, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetCullModeEXT(command, VK_CULL_MODE_NONE);
    vkCmdDraw(command, 6, 1, 0, 0);
    vkCmdSetCullModeEXT(command, VK_CULL_MODE_BACK_BIT);
    vkCmdDraw(command, 6, 1, 6, 0);
    vkCmdDraw(command, 6, 1, 12, 0);
    vkCmdEndRenderingKHR(command);
    assert(command->state == PS5VK_RECORDING);
    /* DXVK's exact global dependency from the attachment writes (host
     * trace: src COLOR_ATTACHMENT_OUTPUT/COLOR_WRITE, dst 0x1400/0x1980). */
    VkMemoryBarrier attachment_writes = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = 0x1980u};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0x1400u,
        0, 1, &attachment_writes, 0, NULL, 0, NULL);
    assert(command->state == PS5VK_RECORDING);
    /* DXVK's exact hand-over: no source access, TRANSFER -> TRANSFER. */
    VkImageMemoryBarrier handover = to_color;
    handover.srcAccessMask = 0;
    handover.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    handover.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    handover.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &handover);
    assert(command->state == PS5VK_RECORDING);
    VkBufferImageCopy2 full = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = FULL_OFFSET, .bufferRowLength = EXTENT, .bufferImageHeight = EXTENT,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {EXTENT, EXTENT, 1}};
    VkCopyImageToBufferInfo2 copy = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .srcImage = image, .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstBuffer = staging, .regionCount = 1, .pRegions = &full};
    vkCmdCopyImageToBuffer2KHR(command, &copy);
    VkBufferImageCopy2 sub = full;
    sub.bufferOffset = SUB_OFFSET; sub.bufferRowLength = SUB_ROW; sub.bufferImageHeight = 0;
    sub.imageOffset = (VkOffset3D){SUB_X, SUB_Y, 0};
    sub.imageExtent = (VkExtent3D){SUB_W, SUB_H, 1};
    copy.pRegions = &sub;
    vkCmdCopyImageToBuffer2KHR(command, &copy);
    assert(command->state == PS5VK_RECORDING);
    /* DXVK's exact finalize barrier: the global publication (src
     * TRANSFER/TRANSFER_WRITE, dst 0x5880/0x3860 including HOST/HOST_READ)
     * and the hand-back with no source access (dst 0x1400/0x1980). One 1.0
     * call carries both, so its destination stage mask is their union. */
    VkMemoryBarrier publish = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = 0x3860u};
    VkImageMemoryBarrier handback = handover;
    handback.srcAccessMask = 0;
    handback.dstAccessMask = 0x1980u;
    handback.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    handback.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, 0x5880u | 0x1400u, 0, 1, &publish, 0, NULL,
        1, &handback);
    assert(command->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);

    /* The recorded stream: prelude, pass (begin, three draws, end), postlude. */
    unsigned end = 0;
    for (unsigned k = 0; k < command->operation_count; ++k)
        if (command->operations[k].type == PS5VK_END_RENDER_PASS) end = k;
    assert(command->operations[0].type == PS5VK_IMAGE_BARRIER &&
           command->operations[1].type == PS5VK_BEGIN_RENDER_PASS && end == 5);
    for (unsigned k = 2; k < 5; ++k) {
        const struct ps5vk_operation *draw = &command->operations[k];
        assert(draw->type == PS5VK_DRAW && draw->vertex_count == 6 &&
               draw->first_vertex == 6u * (k - 2u) && draw->viewport_count == 1 &&
               draw->viewports[0].height == -(float)EXTENT && draw->viewports[0].y == (float)EXTENT &&
               draw->raster.fixed_function_resolved &&
               draw->raster.front_face == VK_FRONT_FACE_CLOCKWISE &&
               draw->raster.cull_mode == (k == 2 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT));
    }
    const struct ps5vk_operation *postlude = &command->operations[end + 1];
    const unsigned postlude_count = command->operation_count - end - 1;

    /* The graphics queue's rule: not a strict CTS sequence, so it is planned
     * as general regions. */
    struct ps5vk_readback_partition strict;
    const int strict_shape = ps5vk_readback_partition(postlude, postlude_count, &strict) ==
            VK_SUCCESS && strict.readback_count == 4 &&
        postlude[strict.readback_first + 2].type == PS5VK_BARRIER &&
        postlude[strict.readback_first + 3].type == PS5VK_BARRIER &&
        postlude[strict.readback_first + 2].buffer_barrier.buffer ==
            postlude[strict.readback_first + 1].copy_destination;
    assert(!strict_shape);
    struct ps5vk_layout_state layouts = {0};
    assert(ps5vk_layout_transition(&layouts, image, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) == VK_SUCCESS);
    struct ps5vk_readback_regions regions;
    unsigned site = 0;
    assert(ps5vk_readback_regions_commands(device, postlude, postlude_count, &layouts, &regions,
        &site) == VK_SUCCESS);
    assert(regions.count == 2 && regions.target[0].region.bufferOffset == FULL_OFFSET &&
           regions.target[1].region.bufferOffset == SUB_OFFSET);
    VkImageLayout final_layout;
    assert(ps5vk_layout_current(&layouts, image, VK_IMAGE_ASPECT_COLOR_BIT, &final_layout) ==
           VK_SUCCESS && final_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    /* Completion: detile each region from a synthetic tiled surface. */
    void *surface, *mapped;
    VkDeviceSize surface_bytes, mapped_bytes;
    assert(ps5vk_image_span(device, image, &surface, &surface_bytes) == VK_SUCCESS);
    assert(ps5vk_buffer_span(device, staging, 0, VK_WHOLE_SIZE, &mapped, &mapped_bytes) == VK_SUCCESS);
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x) {
            const uint32_t value = 0xff000000u | (y << 8) | x;
            memcpy((unsigned char *)surface + ps5vk_rgba8_64k_rx_offset(x, y, EXTENT), &value, 4);
        }
    memset(mapped, 0xcd, STAGING);
    for (unsigned r = 0; r < regions.count; ++r)
        assert(!ps5vk_readback_region_detile(image, regions.target[r].layer_stride,
            &regions.target[r].region, mapped, (size_t)mapped_bytes, surface, (size_t)surface_bytes));
    const unsigned char *bytes = mapped;
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x) {
            uint32_t value;
            memcpy(&value, bytes + FULL_OFFSET + (y * EXTENT + x) * 4u, 4);
            assert(value == (0xff000000u | (y << 8) | x));
        }
    for (uint32_t y = 0; y < SUB_H; ++y)
        for (uint32_t x = 0; x < SUB_W; ++x) {
            uint32_t value;
            memcpy(&value, bytes + SUB_OFFSET + (y * SUB_ROW + x) * 4u, 4);
            assert(value == (0xff000000u | ((SUB_Y + y) << 8) | (SUB_X + x)));
        }
    for (uint32_t b = 0; b < FULL_OFFSET; ++b) assert(bytes[b] == 0xcd);

    vkFreeCommandBuffers(device, pool, 1, &command);
    vkFreeCommandBuffers(device, pool, 1, &init_barriers);
    vkFreeCommandBuffers(device, pool, 1, &init_buffer);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    for (unsigned n = 0; n < 2; ++n) vkDestroyShaderModule(device, modules[n], NULL);
    vkDestroyBuffer(device, staging, NULL);
    vkFreeMemory(device, staging_memory, NULL);
    vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("dxvk render witness trace: pass (host recording and readback planning, no GPU)");
    return 0;
}

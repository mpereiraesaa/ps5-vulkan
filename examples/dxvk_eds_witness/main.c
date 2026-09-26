/* Public-SDK witness for VK_EXT_extended_dynamic_state's dynamic primitive
 * topology and dynamic vertex input binding stride (DXVK262-T10).
 *
 * Pipeline A (vertex + fragment), created TRIANGLE_LIST with PRIMITIVE_
 * TOPOLOGY, VERTEX_INPUT_BINDING_STRIDE and SCISSOR dynamic, draws one quad per
 * 16x16 column of a 128x16 R8G8B8A8_UNORM target; pipeline B adds a geometry
 * stage declaring triangles_adjacency and is created TRIANGLE_LIST_WITH_
 * ADJACENCY. Binding 0 holds a vec2 position (offset 0) and an RGBA8 colour
 * (offset 8); every byte outside the records is 0xAB, so a draw that walks the
 * buffer with the wrong stride reads garbage:
 *   0  A  TRIANGLE_LIST           stride 16   6 vertices
 *   1  A  TRIANGLE_STRIP          stride 16   4
 *   2  A  TRIANGLE_LIST           stride 24   6
 *   3  A  TRIANGLE_STRIP          stride 32   4
 *   4  A  TRIANGLE_FAN            stride 16   4
 *   5  A  TRIANGLE_LIST_WITH_ADJ  stride 16  12  adjacency vertices far away
 *   6  A  TRIANGLE_STRIP_WITH_ADJ stride 16   8  and black: skipped
 *   7  B  TRIANGLE_LIST_WITH_ADJ  stride 16  12  the geometry stage takes the
 *         colour from adjacency vertex 1, the triangle's vertices are grey
 * A wrong topology, stride or adjacency handling leaves a hole, moves a
 * corner or changes the colour, so each column must be exactly its own colour
 * everywhere. The readback runs in its own submission, and every fence wait
 * is 300 ms. */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "dxvk_eds_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 128, HEIGHT = 16, COLUMNS = 8, PIXELS = WIDTH * HEIGHT, REGION = 256 };
static const uint64_t fence_timeout = UINT64_C(300000000);
static const VkPrimitiveTopology topologies[COLUMNS] = {
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY};
static const VkDeviceSize strides[COLUMNS] = {16, 16, 24, 32, 16, 16, 16, 16};
static const uint32_t vertex_counts[COLUMNS] = {6, 4, 6, 4, 4, 12, 8, 12};
static const uint8_t colors[COLUMNS][4] = {
    {0xff, 0x00, 0x00, 0xff}, {0x00, 0xff, 0x00, 0xff}, {0x00, 0x00, 0xff, 0xff},
    {0x40, 0x80, 0xc0, 0xff}, {0xff, 0xff, 0x00, 0xff}, {0xff, 0x00, 0xff, 0xff},
    {0x00, 0xff, 0xff, 0xff}, {0x80, 0x40, 0x20, 0xff}};
static const uint8_t black[4] = {0, 0, 0, 0xff}, grey[4] = {0x10, 0x10, 0x10, 0xff};
static const float far_away[2] = {-7.0f, 9.0f};

/* Column c's vertices: position and colour, in the topology's order. */
static void column_vertices(uint32_t column, float position[12][2], const uint8_t *color[12])
{
    const float x0 = -1.0f + 0.25f * (float)column, x1 = x0 + 0.25f;
    const float list[6][2] = {{x0, -1}, {x1, -1}, {x0, 1}, {x1, -1}, {x1, 1}, {x0, 1}};
    const float strip_order[4][2] = {{x0, -1}, {x1, -1}, {x0, 1}, {x1, 1}};
    const float fan_order[4][2] = {{x0, -1}, {x1, -1}, {x1, 1}, {x0, 1}};
    const uint8_t *own = colors[column];
    for (uint32_t v = 0; v < vertex_counts[column]; ++v) {
        const float *p = NULL;
        const uint8_t *c = own;
        switch (topologies[column]) {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: p = list[v]; break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: p = strip_order[v]; break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: p = fan_order[v]; break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
            /* Even vertices are the triangle, odd ones its adjacency. */
            p = (v & 1u) ? far_away : list[v / 2u];
            if (column == 7) c = (v & 1u) ? own : grey;
            else c = (v & 1u) ? black : own;
            break;
        default: /* TRIANGLE_STRIP_WITH_ADJACENCY */
            p = (v & 1u) ? far_away : strip_order[v / 2u];
            c = (v & 1u) ? black : own;
            break;
        }
        position[v][0] = p[0]; position[v][1] = p[1];
        color[v] = c;
    }
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
    VkDeviceMemory image_memory = VK_NULL_HANDLE, vertex_memory = VK_NULL_HANDLE,
        readback_memory = VK_NULL_HANDLE;
    VkBuffer vertex_buffer = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE, geometry = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE, geometry_pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[2] = {VK_NULL_HANDLE};
    VkFence fence = VK_NULL_HANDLE;
    unsigned completed = 0;

    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &reported};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features2);
    REQUIRE(reported.extendedDynamicState, "extendedDynamicState reported");
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
        .extendedDynamicState = VK_TRUE};
    REQUIRE(features2.features.geometryShader, "geometryShader reported");
    VkPhysicalDeviceFeatures2 enable = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &requested};
    enable.features.geometryShader = VK_TRUE;
    const char *device_extension = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enable, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &device_extension};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    PFN_vkCmdSetPrimitiveTopologyEXT set_topology =
        (PFN_vkCmdSetPrimitiveTopologyEXT)vkGetDeviceProcAddr(device, "vkCmdSetPrimitiveTopologyEXT");
    PFN_vkCmdBindVertexBuffers2EXT bind_buffers2 =
        (PFN_vkCmdBindVertexBuffers2EXT)vkGetDeviceProcAddr(device, "vkCmdBindVertexBuffers2EXT");
    REQUIRE(set_topology && bind_buffers2, "extension entry points resolve");
    ps5log_printf(PS5LOG_MARK, "DXVK_EDS_WITNESS_START width=%u height=%u columns=%u",
        WIDTH, HEIGHT, COLUMNS);

    /* One 256-byte region per column: records at the column's stride, 0xAB
     * everywhere else. */
    VkBufferCreateInfo vertex_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = COLUMNS * REGION, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device, &vertex_info, NULL, &vertex_buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, vertex_buffer, &req);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &allocation, NULL, &vertex_memory));
    TRY(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0));
    {
        uint8_t *mapped = NULL;
        TRY(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
        memset(mapped, 0xab, COLUMNS * REGION);
        for (uint32_t column = 0; column < COLUMNS; ++column) {
            float position[12][2];
            const uint8_t *color[12];
            column_vertices(column, position, color);
            for (uint32_t v = 0; v < vertex_counts[column]; ++v) {
                uint8_t *record = mapped + REGION * column + strides[column] * v;
                memcpy(record, position[v], sizeof(position[v]));
                memcpy(record + 8, color[v], 4);
            }
        }
        VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = vertex_memory, .size = VK_WHOLE_SIZE};
        result = vkFlushMappedMemoryRanges(device, 1, &flush);
        vkUnmapMemory(device, vertex_memory);
        TRY(result);
    }
    VkBufferCreateInfo readback_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = PIXELS * 4u, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device, &readback_info, NULL, &readback));
    vkGetBufferMemoryRequirements(device, readback, &req);
    allocation.allocationSize = req.size;
    TRY(vkAllocateMemory(device, &allocation, NULL, &readback_memory));
    TRY(vkBindBufferMemory(device, readback, readback_memory, 0));

    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {WIDTH, HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    TRY(vkCreateImage(device, &image_info, NULL, &image));
    vkGetImageMemoryRequirements(device, image, &req);
    allocation.allocationSize = req.size;
    TRY(vkAllocateMemory(device, &allocation, NULL, &image_memory));
    TRY(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    TRY(vkCreateImageView(device, &view_info, NULL, &view));
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
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view, .width = WIDTH, .height = HEIGHT,
        .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    VkShaderModuleCreateInfo vertex_module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(dxvk_eds_vert_spirv),
        .pCode = dxvk_eds_vert_spirv};
    VkShaderModuleCreateInfo fragment_module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(dxvk_eds_frag_spirv),
        .pCode = dxvk_eds_frag_spirv};
    TRY(vkCreateShaderModule(device, &vertex_module, NULL, &vertex));
    TRY(vkCreateShaderModule(device, &fragment_module, NULL, &fragment));
    VkShaderModuleCreateInfo geometry_module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(dxvk_eds_geom_spirv), .pCode = dxvk_eds_geom_spirv};
    TRY(vkCreateShaderModule(device, &geometry_module, NULL, &geometry));
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"}};
    /* The static stride is ignored because it is dynamic. */
    VkVertexInputBindingDescription binding = {0, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8}};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0.0f, 0.0f, (float)WIDTH, (float)HEIGHT, 0.0f, 1.0f};
    VkRect2D full = {{0, 0}, {WIDTH, HEIGHT}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &full};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3, .pDynamicStates = dynamic_states};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .pDynamicState = &dynamic,
        .layout = layout, .renderPass = pass, .subpass = 0};
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    /* B: the same state with a geometry stage fed triangles with adjacency. */
    VkPipelineShaderStageCreateInfo geometry_stages[3] = {stages[0],
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = geometry, .pName = "main"},
        stages[1]};
    VkPipelineInputAssemblyStateCreateInfo adjacency = assembly;
    adjacency.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    VkGraphicsPipelineCreateInfo geometry_info = pipeline_info;
    geometry_info.stageCount = 3; geometry_info.pStages = geometry_stages;
    geometry_info.pInputAssemblyState = &adjacency;
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &geometry_info, NULL,
                                  &geometry_pipeline));

    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

    /* 0: the pass, eight draws in eight scissored columns. The clear is a
     * colour no draw produces. */
    TRY(vkBeginCommandBuffer(commands[0], &begin));
    VkClearValue clear = {.color = {.float32 = {1.0f, 0.0f, 1.0f, 0.5f}}};
    VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer, .renderArea = full,
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(commands[0], &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    for (uint32_t column = 0; column < COLUMNS; ++column) {
        VkRect2D scissor = {{(int32_t)(16 * column), 0}, {16, HEIGHT}};
        const VkDeviceSize offset = (VkDeviceSize)REGION * column;
        vkCmdSetScissor(commands[0], 0, 1, &scissor);
        if (column == 7)
            vkCmdBindPipeline(commands[0], VK_PIPELINE_BIND_POINT_GRAPHICS, geometry_pipeline);
        set_topology(commands[0], topologies[column]);
        bind_buffers2(commands[0], 0, 1, &vertex_buffer, &offset, NULL, &strides[column]);
        vkCmdDraw(commands[0], vertex_counts[column], 1, 0, 0);
    }
    vkCmdEndRenderPass(commands[0]);
    TRY(vkEndCommandBuffer(commands[0]));
    /* 1: the colour readback in its own submission. */
    TRY(vkBeginCommandBuffer(commands[1], &begin));
    VkImageMemoryBarrier handover = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &handover);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {WIDTH, HEIGHT, 1}};
    vkCmdCopyImageToBuffer(commands[1], image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback,
                           1, &region);
    VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback, .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host, 0, NULL);
    TRY(vkEndCommandBuffer(commands[1]));
    for (unsigned n = 0; n < 2; ++n) {
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &commands[n]};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
        TRY(vkResetFences(device, 1, &fence));
        ++completed;
    }
    {
        uint8_t *pixels = NULL;
        TRY(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, (void **)&pixels));
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = readback_memory, .size = VK_WHOLE_SIZE};
        result = vkInvalidateMappedMemoryRanges(device, 1, &range);
        for (uint32_t column = 0; column < COLUMNS && result == VK_SUCCESS; ++column) {
            /* Every pixel of the column must equal its first pixel. */
            const uint8_t *first = pixels + 4u * (16u * column);
            uint32_t uniform_mismatches = 0;
            for (uint32_t y = 0; y < HEIGHT; ++y)
                for (uint32_t x = 16 * column; x < 16 * column + 16; ++x)
                    uniform_mismatches += memcmp(pixels + 4u * (y * WIDTH + x), first, 4) != 0;
            ps5log_printf(PS5LOG_MARK,
                "DXVK_EDS_WITNESS_COLUMN column=%u topology=%u stride=%u vertices=%u "
                "rgba=%02x%02x%02x%02x uniform_mismatches=%u",
                column, (unsigned)topologies[column], (unsigned)strides[column],
                vertex_counts[column], first[0], first[1], first[2], first[3],
                uniform_mismatches);
        }
        vkUnmapMemory(device, readback_memory);
        TRY(result);
    }
    ps5log_printf(PS5LOG_MARK, "DXVK_EDS_WITNESS_RESULT submissions=%u fence=complete", completed);

cleanup:
    if (device) {
        vkDeviceWaitIdle(device);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
        if (geometry_pipeline) vkDestroyPipeline(device, geometry_pipeline, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (vertex) vkDestroyShaderModule(device, vertex, NULL);
        if (fragment) vkDestroyShaderModule(device, fragment, NULL);
        if (geometry) vkDestroyShaderModule(device, geometry, NULL);
        if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
        if (pass) vkDestroyRenderPass(device, pass, NULL);
        if (view) vkDestroyImageView(device, view, NULL);
        if (image) vkDestroyImage(device, image, NULL);
        if (vertex_buffer) vkDestroyBuffer(device, vertex_buffer, NULL);
        if (readback) vkDestroyBuffer(device, readback, NULL);
        if (image_memory) vkFreeMemory(device, image_memory, NULL);
        if (vertex_memory) vkFreeMemory(device, vertex_memory, NULL);
        if (readback_memory) vkFreeMemory(device, readback_memory, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    if (failed)
        ps5log_printf(PS5LOG_ERR, "DXVK_EDS_WITNESS_FAILURE result=%d step=%s completed=%u",
            (int)result, failed, completed);
    else
        ps5log_printf(PS5LOG_MARK, "DXVK_EDS_WITNESS_RETIRED resources=clean");
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
    ps5log_close(failed ? "dxvk-eds-witness-failed" : "dxvk-eds-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

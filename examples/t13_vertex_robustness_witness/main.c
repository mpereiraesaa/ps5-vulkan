/* Public-SDK witness for VK_EXT_robustness2 vertex input (DXVK262-T13).
 *
 * The device enables robustBufferAccess, robustBufferAccess2 and
 * nullDescriptor. One render pass over a 48x16 R8G8B8A8_UNORM target draws
 * one flat-coloured triangle per 16x16 column, each column selected by a
 * dynamic scissor; the colour is the binding-0 vec4 attribute:
 *   column 0  binding 0 is VK_NULL_HANDLE (nullDescriptor): every attribute
 *             reads zero;
 *   column 1  binding 0 holds three vertices and the draw starts at vertex 3,
 *             entirely past the buffer (robustBufferAccess2): zero;
 *   column 2  the same buffer from vertex 0: the stored value (the control).
 * Zero means (0,0,0,0) or (0,0,0,1); the witness reports which. The colour
 * readback runs in its own submission, and every fence wait is 300 ms. */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t13_vertex_robustness_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 48, HEIGHT = 16, COLUMNS = 3, PIXELS = WIDTH * HEIGHT };
static const uint64_t fence_timeout = UINT64_C(300000000);
static const float stored[4] = {0.25f, 0.5f, 0.75f, 1.0f};

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
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
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
    VkPhysicalDeviceRobustness2FeaturesEXT reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &reported};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features2);
    REQUIRE(reported.robustBufferAccess2 && reported.nullDescriptor &&
            features2.features.robustBufferAccess, "robustness2 reported");
    VkPhysicalDeviceRobustness2FeaturesEXT requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .robustBufferAccess2 = VK_TRUE, .nullDescriptor = VK_TRUE};
    VkPhysicalDeviceFeatures2 enable = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &requested};
    enable.features.robustBufferAccess = VK_TRUE;
    const char *device_extension = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enable, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &device_extension};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    ps5log_printf(PS5LOG_MARK, "T13_VERTEX_WITNESS_START width=%u height=%u columns=%u",
        WIDTH, HEIGHT, COLUMNS);

    /* Three vertices of the stored value; nothing past them. */
    VkBufferCreateInfo vertex_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 3 * sizeof(stored), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device, &vertex_info, NULL, &vertex_buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, vertex_buffer, &req);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &allocation, NULL, &vertex_memory));
    TRY(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0));
    {
        float *mapped = NULL;
        TRY(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
        for (unsigned v = 0; v < 3; ++v) memcpy(mapped + 4 * v, stored, sizeof(stored));
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
        .codeSize = sizeof(t13_vertex_robustness_vert_spirv),
        .pCode = t13_vertex_robustness_vert_spirv};
    VkShaderModuleCreateInfo fragment_module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t13_vertex_robustness_frag_spirv),
        .pCode = t13_vertex_robustness_frag_spirv};
    TRY(vkCreateShaderModule(device, &vertex_module, NULL, &vertex));
    TRY(vkCreateShaderModule(device, &fragment_module, NULL, &fragment));
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"}};
    VkVertexInputBindingDescription binding = {0, sizeof(stored), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute};
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
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 1, .pDynamicStates = dynamic_states};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .pDynamicState = &dynamic,
        .layout = layout, .renderPass = pass, .subpass = 0};
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));

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

    /* 0: the pass, three draws in three scissored columns. The clear is a
     * colour no draw produces. */
    TRY(vkBeginCommandBuffer(commands[0], &begin));
    VkClearValue clear = {.color = {.float32 = {1.0f, 0.0f, 1.0f, 0.5f}}};
    VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer, .renderArea = full,
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(commands[0], &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize zero = 0;
    const VkBuffer null_buffer = VK_NULL_HANDLE;
    for (uint32_t column = 0; column < COLUMNS; ++column) {
        VkRect2D scissor = {{(int32_t)(16 * column), 0}, {16, HEIGHT}};
        vkCmdSetScissor(commands[0], 0, 1, &scissor);
        vkCmdBindVertexBuffers(commands[0], 0, 1, column ? &vertex_buffer : &null_buffer, &zero);
        vkCmdDraw(commands[0], 3, 1, column == 1 ? 3u : 0u, 0);
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
                "T13_VERTEX_WITNESS_COLUMN column=%u rgba=%02x%02x%02x%02x uniform_mismatches=%u",
                column, first[0], first[1], first[2], first[3], uniform_mismatches);
        }
        vkUnmapMemory(device, readback_memory);
        TRY(result);
    }
    ps5log_printf(PS5LOG_MARK, "T13_VERTEX_WITNESS_RESULT submissions=%u fence=complete", completed);

cleanup:
    if (device) {
        vkDeviceWaitIdle(device);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (vertex) vkDestroyShaderModule(device, vertex, NULL);
        if (fragment) vkDestroyShaderModule(device, fragment, NULL);
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
        ps5log_printf(PS5LOG_ERR, "T13_VERTEX_WITNESS_FAILURE result=%d step=%s completed=%u",
            (int)result, failed, completed);
    else
        ps5log_printf(PS5LOG_MARK, "T13_VERTEX_WITNESS_RETIRED resources=clean");
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
    ps5log_close(failed ? "t13-vertex-witness-failed" : "t13-vertex-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

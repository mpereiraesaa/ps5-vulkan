/* VK_EXT_extended_dynamic_state as the pinned DXVK 2.6.2 first draw uses it
 * (DXVK262-T10).
 *
 * DXVK creates every monolithic graphics pipeline with
 * VIEWPORT_WITH_COUNT + SCISSOR_WITH_COUNT (+ CULL_MODE + FRONT_FACE unless
 * rasterizer discard) and viewportCount = scissorCount = 0
 * (dxvk_graphics.cpp:760-789, dxvk_graphics.h:175), then sets
 * vkCmdSetViewportWithCount/vkCmdSetScissorWithCount/vkCmdSetCullMode/
 * vkCmdSetFrontFace before the draw (dxvk_context.cpp:7025-7123,
 * dxvk_cmdlist.h:1011-1024,1081-1084). On this Vulkan 1.0 device those are the
 * EXT names of VK_EXT_extended_dynamic_state.
 *
 * This test pins: enumeration and the feature query follow the platform bit;
 * vkCreateDevice enforces the registry dependency and the feature; the proc
 * address lookup hides the commands without the extension; the DXVK pipeline
 * shape is accepted only with the feature and malformed shapes are refused;
 * and a recorded draw carries exactly the dynamic values set before it, by
 * value, compared field by field against the same draw recorded with the
 * Vulkan 1.0 static pipeline (the positive control). Only platform discovery
 * and the compiler are mocked. */
#include "vk_internal.h"
#include "vk_command.h"
#include "graphics_formats.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t platform_t09;
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
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = 1u << 20, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features_t09 = platform_t09};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 1u << 20,
        .allocation_granularity = 1, .buffer_image_granularity = 1};
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static unsigned compiled;
static VkResult compile_program(void *context, const struct ps5vk_graphics_key *key, const void **out)
{
    (void)context; assert(key && out);
    ++compiled; *out = &compiled;
    return VK_SUCCESS;
}
static void release_program(void *context, const void *data) { (void)context; (void)data; }
static VkResult create_graphics(VkDevice d, const void *data, uint32_t primitive, void **out)
{
    (void)d; (void)data; (void)primitive;
    *out = malloc(1);
    return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void release_graphics(VkDevice d, void *state) { (void)d; free(state); }

static const char *const EDS = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;

static VkInstance make_instance(int features2)
{
    const char *extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = features2 ? 1u : 0u, .ppEnabledExtensionNames = &extension};
    VkInstance i = VK_NULL_HANDLE;
    assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
    return i;
}
static VkPhysicalDevice physical(VkInstance i)
{
    uint32_t count = 1;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(i, &count, &p) == VK_SUCCESS && p);
    return p;
}
static int lists(VkPhysicalDevice p, const char *name)
{
    uint32_t count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    VkExtensionProperties properties[32];
    assert(count <= 32);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, name)) {
            assert(properties[n].specVersion == VK_EXT_EXTENDED_DYNAMIC_STATE_SPEC_VERSION);
            return 1;
        }
    return 0;
}
static VkResult create(VkPhysicalDevice p, int extension, const void *chain, VkDevice *out)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = chain,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = extension ? 1u : 0u, .ppEnabledExtensionNames = &EDS};
    return vkCreateDevice(p, &info, NULL, out);
}

/* Negotiation: enumeration, the feature query, creation and proc lookup. */
static void negotiation(void)
{
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
        .extendedDynamicState = VK_TRUE};
    VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &query};
    VkDevice d = VK_NULL_HANDLE;

    /* The shipping platform: not listed, feature false, refused. */
    platform_t09 = 0;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    assert(!lists(p, EDS));
    vkGetPhysicalDeviceFeatures2KHR(p, &features2);
    assert(query.extendedDynamicState == VK_FALSE);
    assert(create(p, 1, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);

    platform_t09 = PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE;
    /* Without VK_KHR_get_physical_device_properties2 the registry dependency
     * is unmet on a 1.0 instance. */
    i = make_instance(0);
    p = physical(i);
    assert(lists(p, EDS));
    assert(create(p, 1, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);

    i = make_instance(1);
    p = physical(i);
    assert(lists(p, EDS));
    vkGetPhysicalDeviceFeatures2KHR(p, &features2);
    assert(query.extendedDynamicState == VK_TRUE);
    /* The feature needs the extension. */
    query.pNext = NULL;
    assert(create(p, 0, &query, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    /* A non-boolean value and a duplicated structure are malformed. */
    query.extendedDynamicState = 2u;
    assert(create(p, 1, &query, &d) != VK_SUCCESS && !d);
    query.extendedDynamicState = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT twice = query;
    twice.pNext = &query;
    assert(create(p, 1, &twice, &d) != VK_SUCCESS && !d);
    /* Extension without the feature: the commands exist, but record nothing. */
    assert(create(p, 1, NULL, &d) == VK_SUCCESS && d);
    assert(d->extended_dynamic_state_extension_enabled && !d->extended_dynamic_state_enabled);
    assert(vkGetDeviceProcAddr(d, "vkCmdSetCullModeEXT"));
    assert(vkGetDeviceProcAddr(d, "vkCmdSetStencilOpEXT"));
    assert(vkGetDeviceProcAddr(d, "vkCmdBindVertexBuffers2EXT"));
    /* Vulkan 1.0 has no core names for them. */
    assert(!vkGetDeviceProcAddr(d, "vkCmdSetCullMode"));
    vkDestroyDevice(d, NULL);
    d = VK_NULL_HANDLE;
    assert(create(p, 1, &query, &d) == VK_SUCCESS && d);
    assert(d->extended_dynamic_state_extension_enabled && d->extended_dynamic_state_enabled);
    vkDestroyDevice(d, NULL);
    d = VK_NULL_HANDLE;
    /* No extension: the proc lookup hides every command. */
    assert(create(p, 0, NULL, &d) == VK_SUCCESS && d);
    static const char *const names[] = {"vkCmdSetCullModeEXT", "vkCmdSetFrontFaceEXT",
        "vkCmdSetPrimitiveTopologyEXT", "vkCmdSetViewportWithCountEXT",
        "vkCmdSetScissorWithCountEXT", "vkCmdBindVertexBuffers2EXT",
        "vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthWriteEnableEXT",
        "vkCmdSetDepthCompareOpEXT", "vkCmdSetDepthBoundsTestEnableEXT",
        "vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilOpEXT"};
    for (unsigned n = 0; n < sizeof(names) / sizeof(names[0]); ++n)
        assert(!vkGetDeviceProcAddr(d, names[n]));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

/* The recording half runs on one device with the feature enabled, the host
 * graphics object model and an injected compiler. */
static VkDevice device;
static VkCommandPool pool;
static VkRenderPass pass;
static VkFramebuffer framebuffer;
static VkPipelineLayout layout;
static VkShaderModule modules[2];
static VkInstance instance;
static VkImage image;
static VkImageView view;
static VkDeviceMemory image_memory;

static void open_device(int feature)
{
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
        .extendedDynamicState = feature ? VK_TRUE : VK_FALSE};
    instance = make_instance(1);
    VkPhysicalDevice p = physical(instance);
    assert(create(p, 1, &enable, &device) == VK_SUCCESS);
    device->graphics_enabled = VK_TRUE;
    device->image_requirements = ps5vk_native_image_requirements;
    device->graphics_compiler_context = &compiled;
    device->graphics_acquire = compile_program;
    device->graphics_compiled_release = release_program;
    device->graphics_create = create_graphics;
    device->graphics_release = release_graphics;
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);

    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    assert(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size};
    assert(vkAllocateMemory(device, &allocation, NULL, &image_memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    assert(vkCreateImageView(device, &view_info, NULL, &view) == VK_SUCCESS);
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    assert(vkCreateRenderPass(device, &pass_info, NULL, &pass) == VK_SUCCESS);
    VkFramebufferCreateInfo fb_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &view,
        .width = 64, .height = 64, .layers = 1};
    assert(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    assert(vkCreatePipelineLayout(device, &layout_info, NULL, &layout) == VK_SUCCESS);
    static const uint32_t vs[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 0, 1, 0x6e69616d, 0};
    static const uint32_t fs[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 4, 1, 0x6e69616d, 0};
    for (unsigned n = 0; n < 2; ++n) {
        VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vs), .pCode = n ? fs : vs};
        assert(vkCreateShaderModule(device, &info, NULL, &modules[n]) == VK_SUCCESS);
    }
}

static void close_device(void)
{
    for (unsigned n = 0; n < 2; ++n) vkDestroyShaderModule(device, modules[n], NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    device = VK_NULL_HANDLE;
}

/* The pipeline DXVK builds for Draw(3) with no input layout
 * (DxvkGraphicsPipeline::createOptimizedPipeline, dxvk_graphics.cpp:1388-1437):
 * empty vertex input, TRIANGLE_LIST, depthClampEnable = !depthClip = FALSE,
 * cullMode/frontFace left zero because they are dynamic, a zero viewport
 * state, a depth-stencil state with every test off, one blend attachment. */
static VkResult make_pipeline(const VkDynamicState *states, uint32_t count,
    uint32_t viewport_count, VkPipeline *out)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
    const VkSampleMask mask = 0x1u;
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .pSampleMask = &mask};
    VkViewport viewport = {0, 0, 64, 64, 0, 1};
    VkRect2D scissor = {{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo viewports = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = viewport_count, .pViewports = viewport_count ? &viewport : NULL,
        .scissorCount = viewport_count, .pScissors = viewport_count ? &scissor : NULL};
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xfu};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = count, .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly, .pViewportState = &viewports,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth, .pColorBlendState = &blend,
        .pDynamicState = count ? &dynamic : NULL, .layout = layout, .renderPass = pass,
        .basePipelineIndex = -1};
    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, out);
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer c = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &info, &c) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    return c;
}
static void begin_pass(VkCommandBuffer c)
{
    VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer, .renderArea = {{0, 0}, {64, 64}},
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(c, &info, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_RECORDING);
}
static const struct ps5vk_operation *last_draw(VkCommandBuffer c)
{
    for (unsigned n = c->operation_count; n > 0; --n)
        if (c->operations[n - 1].type == PS5VK_DRAW) return &c->operations[n - 1];
    assert(!"no draw");
    return NULL;
}

static const VkDynamicState dxvk_states[4] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT,
    VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT, VK_DYNAMIC_STATE_CULL_MODE_EXT,
    VK_DYNAMIC_STATE_FRONT_FACE_EXT};

static void pipelines_and_recording(void)
{
    VkPipeline dxvk = VK_NULL_HANDLE, stat = VK_NULL_HANDLE, bad = VK_NULL_HANDLE;
    /* The DXVK shape. */
    assert(make_pipeline(dxvk_states, 4, 0, &dxvk) == VK_SUCCESS && dxvk);
    assert(dxvk->dynamic_eds == (PS5VK_EDS_VIEWPORT_WITH_COUNT | PS5VK_EDS_SCISSOR_WITH_COUNT |
        PS5VK_EDS_CULL_MODE | PS5VK_EDS_FRONT_FACE));
    assert(dxvk->viewport_count == 0 && dxvk->dynamic_viewport && dxvk->dynamic_scissor);
    /* The static values are ignored, and canonical in the object. */
    assert(dxvk->cull_mode == VK_CULL_MODE_NONE && dxvk->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    /* The positive control: the same pipeline with the 1.0 static state. */
    assert(make_pipeline(NULL, 0, 1, &stat) == VK_SUCCESS && stat);
    assert(stat->dynamic_eds == 0 && stat->viewport_count == 1);

    /* Malformed or unexecuted shapes, each refused without an object. */
    const VkDynamicState both_viewports[3] = {VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT};
    assert(make_pipeline(both_viewports, 3, 0, &bad) != VK_SUCCESS && !bad);
    const VkDynamicState one_count[1] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT};
    assert(make_pipeline(one_count, 1, 0, &bad) != VK_SUCCESS && !bad);
    /* *_WITH_COUNT needs zero static counts. */
    assert(make_pipeline(dxvk_states, 4, 1, &bad) != VK_SUCCESS && !bad);
    const VkDynamicState duplicate[2] = {VK_DYNAMIC_STATE_CULL_MODE_EXT,
        VK_DYNAMIC_STATE_CULL_MODE_EXT};
    assert(make_pipeline(duplicate, 2, 1, &bad) != VK_SUCCESS && !bad);
    const VkDynamicState topology[1] = {VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT};
    assert(make_pipeline(topology, 1, 1, &bad) == VK_ERROR_FEATURE_NOT_PRESENT && !bad);
    const VkDynamicState stride[1] = {VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT};
    assert(make_pipeline(stride, 1, 1, &bad) == VK_ERROR_FEATURE_NOT_PRESENT && !bad);
    /* The depth/stencil states are accepted (the draw resolves them). */
    const VkDynamicState depth_states[6] = {VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
        VK_DYNAMIC_STATE_STENCIL_OP_EXT};
    VkPipeline depth_pipeline = VK_NULL_HANDLE;
    assert(make_pipeline(depth_states, 6, 1, &depth_pipeline) == VK_SUCCESS && depth_pipeline);

    /* Positive control: the 1.0 static draw carries the static values. */
    VkCommandBuffer c = begin();
    begin_pass(c);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, stat);
    vkCmdDraw(c, 3, 1, 0, 0);
    assert(c->state == PS5VK_RECORDING);
    const struct ps5vk_operation control = *last_draw(c);
    assert(control.viewport_count == 1 && control.raster.fixed_function_resolved);
    assert(control.raster.cull_mode == VK_CULL_MODE_BACK_BIT &&
           control.raster.front_face == VK_FRONT_FACE_CLOCKWISE);

    /* The DXVK draw with the same values set dynamically produces the same
     * viewport, scissor and raster snapshot as the static control. */
    const VkViewport viewport = {0, 0, 64, 64, 0, 1};
    const VkRect2D scissor = {{0, 0}, {64, 64}};
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, dxvk);
    vkCmdSetViewportWithCountEXT(c, 1, &viewport);
    vkCmdSetScissorWithCountEXT(c, 1, &scissor);
    vkCmdSetCullModeEXT(c, VK_CULL_MODE_BACK_BIT);
    vkCmdSetFrontFaceEXT(c, VK_FRONT_FACE_CLOCKWISE);
    vkCmdDraw(c, 3, 1, 0, 0);
    assert(c->state == PS5VK_RECORDING);
    const struct ps5vk_operation *draw = last_draw(c);
    assert(draw->viewport_count == control.viewport_count);
    assert(!memcmp(draw->viewports, control.viewports, sizeof(VkViewport)));
    assert(!memcmp(draw->scissors, control.scissors, sizeof(VkRect2D)));
    assert(!memcmp(&draw->raster, &control.raster, sizeof(control.raster)));
    assert(draw->vertex_count == 3 && draw->instance_count == 1);

    /* The snapshot is by value: a later setter reaches only later draws. */
    vkCmdSetCullModeEXT(c, VK_CULL_MODE_NONE);
    vkCmdSetFrontFaceEXT(c, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    assert(draw->raster.cull_mode == VK_CULL_MODE_BACK_BIT);
    vkCmdDraw(c, 3, 1, 0, 0);
    draw = last_draw(c);
    assert(draw->raster.cull_mode == VK_CULL_MODE_NONE &&
           draw->raster.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdEndRenderPass(c);
    assert(c->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);

    /* A dynamic state the pipeline declared but the buffer never set fails
     * the draw, one state at a time. */
    for (unsigned missing = 0; missing < 4; ++missing) {
        VkCommandBuffer m = begin();
        begin_pass(m);
        vkCmdBindPipeline(m, VK_PIPELINE_BIND_POINT_GRAPHICS, dxvk);
        if (missing != 0) vkCmdSetViewportWithCountEXT(m, 1, &viewport);
        if (missing != 1) vkCmdSetScissorWithCountEXT(m, 1, &scissor);
        if (missing != 2) vkCmdSetCullModeEXT(m, VK_CULL_MODE_NONE);
        if (missing != 3) vkCmdSetFrontFaceEXT(m, VK_FRONT_FACE_CLOCKWISE);
        assert(m->state == PS5VK_RECORDING);
        vkCmdDraw(m, 3, 1, 0, 0);
        assert(m->state == PS5VK_INVALID);
        vkFreeCommandBuffers(device, pool, 1, &m);
    }
    /* The 1.0 setters do not satisfy the *_WITH_COUNT states. */
    VkCommandBuffer v = begin();
    begin_pass(v);
    vkCmdBindPipeline(v, VK_PIPELINE_BIND_POINT_GRAPHICS, dxvk);
    vkCmdSetViewport(v, 0, 1, &viewport);
    vkCmdSetScissor(v, 0, 1, &scissor);
    vkCmdSetCullModeEXT(v, VK_CULL_MODE_NONE);
    vkCmdSetFrontFaceEXT(v, VK_FRONT_FACE_CLOCKWISE);
    vkCmdDraw(v, 3, 1, 0, 0);
    assert(v->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &v);

    /* Malformed setter arguments poison the buffer and store nothing. */
    struct { int which; } cases[] = {{0}, {1}, {2}, {3}, {4}, {5}};
    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
        VkCommandBuffer b = begin();
        const VkViewport negative = {0, 0, -1, 64, 0, 1};
        switch (cases[n].which) {
        case 0: vkCmdSetViewportWithCountEXT(b, 0, &viewport); break;
        /* Without multiViewport the count is exactly one. */
        case 1: vkCmdSetViewportWithCountEXT(b, 2, (const VkViewport[2]){viewport, viewport}); break;
        case 2: vkCmdSetViewportWithCountEXT(b, 1, &negative); break;
        case 3: vkCmdSetCullModeEXT(b, 4u); break;
        case 4: vkCmdSetFrontFaceEXT(b, (VkFrontFace)2); break;
        case 5: vkCmdSetStencilOpEXT(b, 0, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                    VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS); break;
        }
        assert(b->state == PS5VK_INVALID && !(b->eds_valid));
        vkFreeCommandBuffers(device, pool, 1, &b);
    }

    /* Depth and stencil state: resolved into the snapshot; a dynamically
     * enabled stencil test without a stencil attachment executes as disabled,
     * and a dynamically enabled depth bounds test is refused. */
    VkCommandBuffer d = begin();
    begin_pass(d);
    vkCmdBindPipeline(d, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_pipeline);
    vkCmdSetDepthTestEnableEXT(d, VK_TRUE);
    vkCmdSetDepthWriteEnableEXT(d, VK_TRUE);
    vkCmdSetDepthCompareOpEXT(d, VK_COMPARE_OP_LESS_OR_EQUAL);
    vkCmdSetDepthBoundsTestEnableEXT(d, VK_FALSE);
    vkCmdSetStencilTestEnableEXT(d, VK_TRUE);
    vkCmdSetStencilOpEXT(d, VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_KEEP,
        VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL);
    /* One face set: the state is not complete yet. */
    vkCmdDraw(d, 3, 1, 0, 0);
    assert(d->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &d);
    d = begin();
    begin_pass(d);
    vkCmdBindPipeline(d, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_pipeline);
    vkCmdSetDepthTestEnableEXT(d, VK_TRUE);
    vkCmdSetDepthWriteEnableEXT(d, VK_TRUE);
    vkCmdSetDepthCompareOpEXT(d, VK_COMPARE_OP_LESS_OR_EQUAL);
    vkCmdSetDepthBoundsTestEnableEXT(d, VK_FALSE);
    vkCmdSetStencilTestEnableEXT(d, VK_TRUE);
    vkCmdSetStencilOpEXT(d, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_ZERO,
        VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INVERT, VK_COMPARE_OP_EQUAL);
    vkCmdDraw(d, 3, 1, 0, 0);
    assert(d->state == PS5VK_RECORDING);
    draw = last_draw(d);
    assert(draw->raster.depth_test && draw->raster.depth_write &&
           draw->raster.depth_compare == VK_COMPARE_OP_LESS_OR_EQUAL);
    assert(!draw->raster.stencil_test);
    assert(draw->raster.stencil_front.failOp == VK_STENCIL_OP_ZERO &&
           draw->raster.stencil_back.passOp == VK_STENCIL_OP_REPLACE &&
           draw->raster.stencil_back.depthFailOp == VK_STENCIL_OP_INVERT &&
           draw->raster.stencil_front.compareOp == VK_COMPARE_OP_EQUAL);
    vkCmdSetDepthBoundsTestEnableEXT(d, VK_TRUE);
    vkCmdDraw(d, 3, 1, 0, 0);
    assert(d->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &d);

    /* vkCmdBindVertexBuffers2EXT: sizes must fit, strides need a pipeline
     * that declared them dynamic, which none can. */
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VkBuffer buffer = VK_NULL_HANDLE;
    assert(vkCreateBuffer(device, &buffer_info, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    const VkDeviceSize offset = 16, fits = 240, too_big = 241, stride_value = 16;
    VkCommandBuffer b = begin();
    vkCmdBindVertexBuffers2EXT(b, 0, 1, &buffer, &offset, &fits, NULL);
    assert(b->state == PS5VK_RECORDING && b->vertices[0].buffer == buffer &&
           b->vertices[0].offset == 16);
    const VkDeviceSize whole = VK_WHOLE_SIZE;
    vkCmdBindVertexBuffers2EXT(b, 1, 1, &buffer, &offset, &whole, NULL);
    vkCmdBindVertexBuffers2EXT(b, 2, 1, &buffer, &offset, NULL, NULL);
    assert(b->state == PS5VK_RECORDING && b->vertices[2].buffer == buffer);
    vkCmdBindVertexBuffers2EXT(b, 0, 1, &buffer, &offset, &too_big, NULL);
    assert(b->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &b);
    b = begin();
    vkCmdBindVertexBuffers2EXT(b, 0, 1, &buffer, &offset, &fits, &stride_value);
    assert(b->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &b);

    /* Reset clears every extended dynamic state. */
    b = begin();
    vkCmdSetCullModeEXT(b, VK_CULL_MODE_BACK_BIT);
    vkCmdSetViewportWithCountEXT(b, 1, &viewport);
    assert(b->eds_valid && b->viewport_with_count == 1);
    assert(vkResetCommandBuffer(b, 0) == VK_SUCCESS);
    assert(!b->eds_valid && !b->viewport_with_count && b->cull_mode == VK_CULL_MODE_NONE);
    vkFreeCommandBuffers(device, pool, 1, &b);
    vkFreeCommandBuffers(device, pool, 1, &c);

    vkDestroyPipeline(device, dxvk, NULL);
    vkDestroyPipeline(device, stat, NULL);
    vkDestroyPipeline(device, depth_pipeline, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
}

/* The extension without its feature: pipelines cannot declare the states
 * and the setters record nothing. */
static void feature_disabled(void)
{
    VkPipeline bad = VK_NULL_HANDLE;
    assert(make_pipeline(dxvk_states, 4, 0, &bad) == VK_ERROR_FEATURE_NOT_PRESENT && !bad);
    VkCommandBuffer c = begin();
    vkCmdSetCullModeEXT(c, VK_CULL_MODE_NONE);
    assert(c->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &c);
}

int main(void)
{
    negotiation();
    platform_t09 = PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE;
    open_device(1);
    pipelines_and_recording();
    close_device();
    open_device(0);
    feature_disabled();
    close_device();
    puts("dxvk render extended dynamic state tests passed");
    return 0;
}

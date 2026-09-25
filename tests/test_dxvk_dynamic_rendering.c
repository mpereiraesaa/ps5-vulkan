/* VK_KHR_dynamic_rendering (with its Vulkan 1.0 registry dependency
 * VK_KHR_depth_stencil_resolve) as the pinned DXVK 2.6.2 first frame uses it
 * (DXVK262-T10).
 *
 * Measured on DXVK's first frame against a host driver: one
 * vkCmdBeginRendering with flags 0, renderArea (0,0,64,64), layerCount 1,
 * viewMask 0 and one colour attachment in COLOR_ATTACHMENT_OPTIMAL with
 * loadOp CLEAR (the ClearRenderTargetView colour), storeOp STORE and no
 * resolve; a pipeline with renderPass = VK_NULL_HANDLE and
 * VkPipelineRenderingCreateInfo{1 x R8G8B8A8_UNORM}, dynamic VIEWPORT/SCISSOR
 * _WITH_COUNT + CULL_MODE + FRONT_FACE, a depth-stencil state with the test on
 * but no depth attachment, and every stage chaining its own
 * VkShaderModuleCreateInfo; then the four setters, vkCmdDraw(3,1,0,0) and
 * vkCmdEndRendering.
 *
 * The positive control is the same frame recorded through vkCmdBeginRenderPass
 * with the equivalent render pass: the begin, draw and end operations must
 * agree field by field (render area, clear values, the pass's attachment
 * descriptions and subpass roles, the framebuffer's views and extent), so the
 * queue and the native attachment plans receive the pass they already
 * execute. Refusals, negotiation, the resolve dependency, the stage chain rule
 * and object lifetime are pinned as well. Only platform discovery and the
 * compiler are mocked. */
#include "vk_internal.h"
#include "vk_command.h"
#include "vk_framebuffer.h"
#include "graphics_formats.h"
#include "graphics_program.h"
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
        .max_allocation = 1u << 22, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features = PS5VK_FEATURE_MULTIVIEW,
        .supported_features_t09 = platform_t09};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 1u << 22,
        .allocation_granularity = 1, .buffer_image_granularity = 1};
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static const uint32_t all_bits = PS5VK_T09_FEATURE_MAINTENANCE2 |
    PS5VK_T09_FEATURE_CREATE_RENDERPASS2 | PS5VK_T09_FEATURE_DEPTH_STENCIL_RESOLVE |
    PS5VK_T09_FEATURE_DYNAMIC_RENDERING | PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE;
static const char *const chain[] = {VK_KHR_MULTIVIEW_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_2_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
    VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};

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
    VkExtensionProperties properties[32];
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS && count <= 32);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, name)) return 1;
    return 0;
}
/* The first `extensions` names of the chain, and the two features. */
static VkResult create(VkPhysicalDevice p, uint32_t extensions, VkBool32 dynamic,
    VkDevice *out)
{
    float priority = 1.0f;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
        .extendedDynamicState = extensions == 6 ? VK_TRUE : VK_FALSE};
    VkPhysicalDeviceDynamicRenderingFeatures features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = &eds, .dynamicRendering = dynamic};
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = extensions, .ppEnabledExtensionNames = chain};
    return vkCreateDevice(p, &info, NULL, out);
}

static void negotiation(void)
{
    VkDevice d = VK_NULL_HANDLE;
    VkPhysicalDeviceDynamicRenderingFeatures query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceDepthStencilResolveProperties resolve = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
        .supportedDepthResolveModes = 0xffu, .independentResolve = VK_TRUE};
    VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &query};
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &resolve};

    /* Shipping: nothing listed, the feature false, the extension refused. */
    platform_t09 = PS5VK_T09_FEATURE_MAINTENANCE2 | PS5VK_T09_FEATURE_CREATE_RENDERPASS2;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    assert(!lists(p, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) &&
           !lists(p, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME));
    vkGetPhysicalDeviceFeatures2KHR(p, &features2);
    vkGetPhysicalDeviceProperties2KHR(p, &properties2);
    assert(!query.dynamicRendering && !resolve.supportedDepthResolveModes &&
           !resolve.supportedStencilResolveModes && !resolve.independentResolve);
    assert(create(p, 4, VK_FALSE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    /* The neutral feature structure is still accepted without the extension,
     * and asking for the feature is refused. */
    assert(create(p, 3, VK_FALSE, &d) == VK_SUCCESS && d);
    vkDestroyDevice(d, NULL); d = VK_NULL_HANDLE;
    assert(create(p, 3, VK_TRUE, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);

    /* The dynamic-rendering bit alone is not enough: its dependency chain has
     * to be reported beneath it. */
    platform_t09 = PS5VK_T09_FEATURE_DYNAMIC_RENDERING | PS5VK_T09_FEATURE_MAINTENANCE2 |
        PS5VK_T09_FEATURE_CREATE_RENDERPASS2;
    i = make_instance(1);
    p = physical(i);
    assert(!lists(p, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME));
    vkDestroyInstance(i, NULL);

    platform_t09 = all_bits;
    i = make_instance(1);
    p = physical(i);
    assert(lists(p, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) &&
           lists(p, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME));
    query.dynamicRendering = VK_FALSE;
    vkGetPhysicalDeviceFeatures2KHR(p, &features2);
    vkGetPhysicalDeviceProperties2KHR(p, &properties2);
    assert(query.dynamicRendering == VK_TRUE);
    assert(resolve.supportedDepthResolveModes == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT &&
           resolve.supportedStencilResolveModes == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT &&
           !resolve.independentResolveNone && !resolve.independentResolve);
    /* Every registry dependency must be enabled with it. */
    static const char *const without_resolve[] = {VK_KHR_MULTIVIEW_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_2_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 4, .ppEnabledExtensionNames = without_resolve};
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    static const char *const resolve_alone[] = {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME};
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = resolve_alone;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    /* The extension without the feature: the commands exist and refuse. */
    assert(create(p, 5, VK_FALSE, &d) == VK_SUCCESS && d);
    assert(d->dynamic_rendering_extension_enabled && !d->dynamic_rendering_enabled);
    assert(vkGetDeviceProcAddr(d, "vkCmdBeginRenderingKHR") &&
           vkGetDeviceProcAddr(d, "vkCmdEndRenderingKHR"));
    assert(!vkGetDeviceProcAddr(d, "vkCmdBeginRendering"));
    vkDestroyDevice(d, NULL); d = VK_NULL_HANDLE;
    assert(create(p, 3, VK_FALSE, &d) == VK_SUCCESS && d);
    assert(!vkGetDeviceProcAddr(d, "vkCmdBeginRenderingKHR"));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

static VkInstance instance;
static VkDevice device;
static VkCommandPool pool;
static VkImage image;
static VkDeviceMemory memory;
static VkImageView view;
static VkPipelineLayout layout;
static VkShaderModule modules[2];
static const uint32_t vs[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 0, 1, 0x6e69616d, 0};
static const uint32_t fs[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 4, 1, 0x6e69616d, 0};

static unsigned compiled;
static VkResult compile_program(void *context, const struct ps5vk_graphics_key *key, const void **out)
{
    (void)context;
    assert(key->color_attachment_count == 1 && key->color_format[0] == VK_FORMAT_R8G8B8A8_UNORM &&
           key->samples == VK_SAMPLE_COUNT_1_BIT);
    ++compiled; *out = &compiled;
    return VK_SUCCESS;
}
static void release_program(void *context, const void *data) { (void)context; (void)data; }
static VkResult create_graphics(VkDevice d, const void *data, uint32_t primitive, void **out)
{ (void)d; (void)data; (void)primitive; *out = malloc(1); return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release_graphics(VkDevice d, void *state) { (void)d; free(state); }

static void open_device(VkBool32 dynamic)
{
    platform_t09 = all_bits;
    instance = make_instance(1);
    assert(create(physical(instance), 6, dynamic, &device) == VK_SUCCESS);
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
    /* DXVK's render target: R8G8B8A8_UNORM 64x64, TRANSFER_SRC|TRANSFER_DST|
     * COLOR_ATTACHMENT (the MUTABLE_FORMAT flag belongs to the format slice). */
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    assert(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size};
    assert(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    assert(vkCreateImageView(device, &view_info, NULL, &view) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    assert(vkCreatePipelineLayout(device, &layout_info, NULL, &layout) == VK_SUCCESS);
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
    if (view) vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    view = VK_NULL_HANDLE;
}
static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer c = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &info, &c) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    return c;
}

/* DXVK's pipeline, with renderPass = NULL (rendering) or the control pass. */
static VkResult make_pipeline(VkRenderPass pass, const void *rendering, int inline_modules,
    VkPipeline *out)
{
    VkShaderModuleCreateInfo inline_info[2];
    for (unsigned n = 0; n < 2; ++n)
        inline_info[n] = (VkShaderModuleCreateInfo){VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            NULL, 0, sizeof(vs), n ? fs : vs};
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = inline_modules ? &inline_info[0] : NULL,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = inline_modules ? &inline_info[1] : NULL,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    const VkSampleMask mask = 0xffffffffu;
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .pSampleMask = &mask};
    VkPipelineViewportStateCreateInfo viewports = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xfu};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    const VkDynamicState states[4] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT, VK_DYNAMIC_STATE_CULL_MODE_EXT,
        VK_DYNAMIC_STATE_FRONT_FACE_EXT};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 4, .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = rendering, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly, .pViewportState = &viewports,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth, .pColorBlendState = &blend, .pDynamicState = &dynamic,
        .layout = layout, .renderPass = pass, .basePipelineIndex = -1};
    *out = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, out);
}

static const VkFormat rt_format = VK_FORMAT_R8G8B8A8_UNORM;
static const VkPipelineRenderingCreateInfo dxvk_rendering = {
    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &rt_format,
    VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
static VkRenderingAttachmentInfo dxvk_color(void)
{
    VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    a.clearValue.color.float32[0] = 1.0f;
    a.clearValue.color.float32[3] = 1.0f;
    return a;
}
static VkRenderingInfo dxvk_info(const VkRenderingAttachmentInfo *color)
{
    return (VkRenderingInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {64, 64}}, .layerCount = 1, .colorAttachmentCount = 1,
        .pColorAttachments = color};
}
static void dxvk_state(VkCommandBuffer c, VkPipeline pipeline)
{
    const VkViewport viewport = {0, 0, 64, 64, 0, 1};
    const VkRect2D scissor = {{0, 0}, {64, 64}};
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewportWithCountEXT(c, 1, &viewport);
    vkCmdSetScissorWithCountEXT(c, 1, &scissor);
    vkCmdSetCullModeEXT(c, VK_CULL_MODE_BACK_BIT);
    vkCmdSetFrontFaceEXT(c, VK_FRONT_FACE_CLOCKWISE);
}

static void same_pass(VkRenderPass a, VkRenderPass b)
{
    assert(a->attachment_count == b->attachment_count && a->subpass_count == 1 &&
           b->subpass_count == 1 && !a->dependency_count && !b->dependency_count);
    assert(!memcmp(a->attachments, b->attachments,
        a->attachment_count * sizeof(*a->attachments)));
    const struct ps5vk_subpass *x = &a->subpasses[0], *y = &b->subpasses[0];
    assert(x->color_count == y->color_count && x->resolve_count == y->resolve_count &&
           !memcmp(x->color, y->color, x->color_count * sizeof(*x->color)) &&
           x->depth.attachment == y->depth.attachment && !x->input_count && !y->input_count &&
           !x->preserve_count && !y->preserve_count);
}
static void same_framebuffer(VkFramebuffer a, VkFramebuffer b)
{
    assert(a->width == b->width && a->height == b->height &&
           a->attachment_count == b->attachment_count && a->color_count == b->color_count &&
           a->depth_attachment == b->depth_attachment && !a->imageless && !b->imageless);
    for (uint32_t i = 0; i < a->attachment_count; ++i)
        assert(a->attachments[i] == b->attachments[i] && a->formats[i] == b->formats[i] &&
               a->samples[i] == b->samples[i]);
    for (uint32_t i = 0; i < a->color_count; ++i)
        assert(a->color_attachments[i] == b->color_attachments[i]);
}

static void first_frame(void)
{
    VkPipeline dynamic_pipeline = VK_NULL_HANDLE, pass_pipeline = VK_NULL_HANDLE;
    /* DXVK's pipeline, including the redundant inline module chain. */
    assert(make_pipeline(VK_NULL_HANDLE, &dxvk_rendering, 1, &dynamic_pipeline) == VK_SUCCESS);
    assert(dynamic_pipeline->dynamic_rendering && dynamic_pipeline->color_attachment_count == 1 &&
           dynamic_pipeline->color_format[0] == VK_FORMAT_R8G8B8A8_UNORM &&
           dynamic_pipeline->depth_format == VK_FORMAT_UNDEFINED);

    /* The positive control: the equivalent render pass and framebuffer. */
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass pass = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(device, &pass_info, NULL, &pass) == VK_SUCCESS);
    VkFramebufferCreateInfo fb_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &view,
        .width = 64, .height = 64, .layers = 1};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    assert(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer) == VK_SUCCESS);
    assert(make_pipeline(pass, NULL, 0, &pass_pipeline) == VK_SUCCESS && !pass_pipeline->dynamic_rendering);

    VkRenderingAttachmentInfo color = dxvk_color();
    VkRenderingInfo rendering = dxvk_info(&color);
    VkCommandBuffer dynamic = begin();
    vkCmdBeginRenderingKHR(dynamic, &rendering);
    assert(dynamic->state == PS5VK_RECORDING && dynamic->dynamic_rendering && dynamic->render_pass);
    dxvk_state(dynamic, dynamic_pipeline);
    vkCmdDraw(dynamic, 3, 1, 0, 0);
    vkCmdEndRenderingKHR(dynamic);
    assert(dynamic->state == PS5VK_RECORDING && !dynamic->dynamic_rendering && !dynamic->render_pass);
    assert(vkEndCommandBuffer(dynamic) == VK_SUCCESS);

    VkClearValue clear = color.clearValue;
    VkRenderPassBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer, .renderArea = {{0, 0}, {64, 64}},
        .clearValueCount = 1, .pClearValues = &clear};
    VkCommandBuffer control = begin();
    vkCmdBeginRenderPass(control, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
    dxvk_state(control, pass_pipeline);
    vkCmdDraw(control, 3, 1, 0, 0);
    vkCmdEndRenderPass(control);
    assert(vkEndCommandBuffer(control) == VK_SUCCESS);

    assert(dynamic->operation_count == 3 && control->operation_count == 3);
    const struct ps5vk_operation *db = &dynamic->operations[0], *cb = &control->operations[0];
    assert(db->type == PS5VK_BEGIN_RENDER_PASS && cb->type == PS5VK_BEGIN_RENDER_PASS);
    assert(db->owned_payload && db->render_pass != pass && db->framebuffer != framebuffer);
    same_pass(db->render_pass, cb->render_pass);
    same_framebuffer(db->framebuffer, cb->framebuffer);
    assert(!memcmp(&db->render_area, &cb->render_area, sizeof(db->render_area)) &&
           db->render_pass_contents == cb->render_pass_contents && db->subpass == 0 &&
           db->clear_count == cb->clear_count &&
           !memcmp(db->clears, cb->clears, db->clear_count * sizeof(VkClearValue)));
    const struct ps5vk_operation *dd = &dynamic->operations[1], *cd = &control->operations[1];
    assert(dd->type == PS5VK_DRAW && cd->type == PS5VK_DRAW);
    assert(dd->render_pass == db->render_pass && dd->framebuffer == db->framebuffer);
    assert(dd->vertex_count == 3 && dd->instance_count == 1 && dd->viewport_count == 1 &&
           !memcmp(dd->viewports, cd->viewports, sizeof(VkViewport)) &&
           !memcmp(&dd->raster, &cd->raster, sizeof(dd->raster)));
    assert(dynamic->operations[2].type == PS5VK_END_RENDER_PASS &&
           dynamic->operations[2].render_pass == db->render_pass);

    /* A pipeline of the other kind does not draw in either instance. */
    VkCommandBuffer mixed = begin();
    vkCmdBeginRenderingKHR(mixed, &rendering);
    dxvk_state(mixed, pass_pipeline);
    vkCmdDraw(mixed, 3, 1, 0, 0);
    assert(mixed->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &mixed);
    mixed = begin();
    vkCmdBeginRenderPass(mixed, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
    dxvk_state(mixed, dynamic_pipeline);
    vkCmdDraw(mixed, 3, 1, 0, 0);
    assert(mixed->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &mixed);
    /* And each instance closes only with its own command. */
    mixed = begin();
    vkCmdBeginRenderingKHR(mixed, &rendering);
    vkCmdEndRenderPass(mixed);
    assert(mixed->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &mixed);
    mixed = begin();
    vkCmdBeginRenderPass(mixed, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderingKHR(mixed);
    assert(mixed->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &mixed);
    /* A standalone clear (DXVK's flushed ClearRenderTargetView): begin/end
     * with nothing between is a legal, recorded instance. */
    mixed = begin();
    vkCmdBeginRenderingKHR(mixed, &rendering);
    vkCmdEndRenderingKHR(mixed);
    assert(mixed->state == PS5VK_RECORDING && mixed->operation_count == 2);
    vkFreeCommandBuffers(device, pool, 1, &mixed);

    /* Ownership: the instance's pass and framebuffer are the begin's payload;
     * destroying the view it names invalidates the recording, and a reset
     * frees the payload (ASan checks the leak). */
    VkCommandBuffer owned = begin();
    vkCmdBeginRenderingKHR(owned, &rendering);
    vkCmdEndRenderingKHR(owned);
    assert(owned->state == PS5VK_RECORDING);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyImageView(device, view, NULL);
    view = VK_NULL_HANDLE;
    assert(owned->state == PS5VK_INVALID && dynamic->state == PS5VK_INVALID);
    assert(vkResetCommandBuffer(owned, 0) == VK_SUCCESS && !owned->operation_count);

    vkFreeCommandBuffers(device, pool, 1, &owned);
    vkFreeCommandBuffers(device, pool, 1, &dynamic);
    vkFreeCommandBuffers(device, pool, 1, &control);
    vkDestroyPipeline(device, dynamic_pipeline, NULL);
    vkDestroyPipeline(device, pass_pipeline, NULL);
    vkDestroyRenderPass(device, pass, NULL);
}

static void refusals(void)
{
    /* Each malformed or unexecuted begin poisons the buffer and records
     * nothing. */
    for (unsigned n = 0; n < 12; ++n) {
        VkRenderingAttachmentInfo color = dxvk_color();
        VkRenderingInfo info = dxvk_info(&color);
        VkRenderingAttachmentInfo second[2] = {color, color};
        switch (n) {
        case 0: info.flags = VK_RENDERING_SUSPENDING_BIT; break;
        case 1: info.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT; break;
        case 2: info.viewMask = 1u; break;
        case 3: info.layerCount = 2; break;
        case 4: color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT; break;
        case 5: color.imageView = VK_NULL_HANDLE; break;
        case 6: color.storeOp = VK_ATTACHMENT_STORE_OP_NONE; break;
        case 7: color.imageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; break;
        case 8: info.renderArea.extent.width = 65; break;
        case 9: info.pNext = &info; break;
        case 10: info.renderArea.extent.height = 0; break;
        case 11: second[1].imageView = VK_NULL_HANDLE; info.colorAttachmentCount = 2;
                 info.pColorAttachments = second; break;
        }
        VkCommandBuffer c = begin();
        vkCmdBeginRenderingKHR(c, &info);
        assert(c->state == PS5VK_INVALID && !c->operation_count && !c->render_pass);
        vkFreeCommandBuffers(device, pool, 1, &c);
    }
    /* Nested and unmatched instances. */
    VkRenderingAttachmentInfo color = dxvk_color();
    VkRenderingInfo info = dxvk_info(&color);
    VkCommandBuffer c = begin();
    vkCmdBeginRenderingKHR(c, &info);
    vkCmdBeginRenderingKHR(c, &info);
    assert(c->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &c);
    c = begin();
    vkCmdEndRenderingKHR(c);
    assert(c->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &c);
    c = begin();
    vkCmdBeginRenderingKHR(c, &info);
    assert(vkEndCommandBuffer(c) != VK_SUCCESS);
    vkFreeCommandBuffers(device, pool, 1, &c);

    /* Pipeline shapes: no rendering info, a hole, a view mask, differing
     * depth and stencil formats, and an inline module that does not match the
     * stage's module. */
    VkPipeline bad = VK_NULL_HANDLE;
    assert(make_pipeline(VK_NULL_HANDLE, NULL, 0, &bad) != VK_SUCCESS && !bad);
    const VkFormat hole[2] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM};
    VkPipelineRenderingCreateInfo shape = dxvk_rendering;
    shape.colorAttachmentCount = 2; shape.pColorAttachmentFormats = hole;
    assert(make_pipeline(VK_NULL_HANDLE, &shape, 0, &bad) != VK_SUCCESS && !bad);
    shape = dxvk_rendering; shape.viewMask = 1;
    assert(make_pipeline(VK_NULL_HANDLE, &shape, 0, &bad) != VK_SUCCESS && !bad);
    shape = dxvk_rendering; shape.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    shape.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    assert(make_pipeline(VK_NULL_HANDLE, &shape, 0, &bad) != VK_SUCCESS && !bad);
    /* The redundant module chain must carry the stage module's exact words. */
    VkShaderModule swapped = modules[0];
    modules[0] = modules[1];
    assert(make_pipeline(VK_NULL_HANDLE, &dxvk_rendering, 1, &bad) == VK_ERROR_FEATURE_NOT_PRESENT && !bad);
    modules[0] = swapped;
}

static void feature_disabled(void)
{
    VkRenderingAttachmentInfo color = dxvk_color();
    VkRenderingInfo info = dxvk_info(&color);
    VkCommandBuffer c = begin();
    vkCmdBeginRenderingKHR(c, &info);
    assert(c->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &c);
    VkPipeline bad = VK_NULL_HANDLE;
    assert(make_pipeline(VK_NULL_HANDLE, &dxvk_rendering, 1, &bad) != VK_SUCCESS && !bad);
}

/* VK_KHR_depth_stencil_resolve on a version-2 render pass: only the form
 * that names no resolve attachment is accepted. */
static void depth_stencil_resolve(void)
{
    VkAttachmentDescription2 attachment = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference2 reference = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
        .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 unused = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
        .attachment = VK_ATTACHMENT_UNUSED};
    VkSubpassDescriptionDepthStencilResolve resolve = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
        .depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
        .stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT};
    VkSubpassDescription2 subpass = {.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
        .pNext = &resolve, .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo2 info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1,
        .pSubpasses = &subpass};
    VkRenderPass pass = VK_NULL_HANDLE;
    assert(vkCreateRenderPass2KHR(device, &info, NULL, &pass) == VK_SUCCESS && pass);
    vkDestroyRenderPass(device, pass, NULL); pass = VK_NULL_HANDLE;
    resolve.pDepthStencilResolveAttachment = &unused;
    assert(vkCreateRenderPass2KHR(device, &info, NULL, &pass) == VK_SUCCESS && pass);
    vkDestroyRenderPass(device, pass, NULL); pass = VK_NULL_HANDLE;
    resolve.pDepthStencilResolveAttachment = &reference;
    assert(vkCreateRenderPass2KHR(device, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT && !pass);
    /* The caller's chain is never modified. */
    assert(subpass.pNext == &resolve);
}

int main(void)
{
    negotiation();
    open_device(VK_TRUE);
    first_frame();
    refusals();
    depth_stencil_resolve();
    close_device();
    open_device(VK_FALSE);
    feature_disabled();
    close_device();
    puts("dxvk dynamic rendering tests passed");
    return 0;
}

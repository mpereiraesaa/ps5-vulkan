/* VK_KHR_maintenance1 as the pinned DXVK 2.6.2 needs it (DXVK262-T10).
 *
 * DXVK always draws with a D3D-style y-flipped viewport: measured on its first
 * frame against a host driver, vkCmdSetViewportWithCount(1, {x 0, y 64,
 * width 64, height -64, 0, 1}) for a 64x64 target. A negative height is
 * VK_KHR_maintenance1 behaviour on a Vulkan 1.0 device.
 *
 * Pinned here: enumeration and creation follow the platform bit (the
 * extension has no registry dependency); vkTrimCommandPoolKHR is reachable
 * only with the extension and is a no-op on a valid pool; a negative height
 * is refused without the extension and accepted with it, in vkCmdSetViewport,
 * vkCmdSetViewportWithCountEXT and a static pipeline viewport, and the draw
 * snapshot carries it unchanged; a zero height stays refused; the 3D
 * 2D_ARRAY_COMPATIBLE flag is refused by the image format query, which the
 * extension permits; and the console format table reports the TRANSFER_SRC and
 * TRANSFER_DST bits on the RGBA8 colour format DXVK copies. Only platform
 * discovery and the compiler are mocked. */
#include "vk_internal.h"
#include "vk_command.h"
#include "graphics_formats.h"
#include "texture_format.h"
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
        .supported_features_t09 = platform_t09};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 1u << 22,
        .allocation_granularity = 1, .buffer_image_granularity = 1};
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static const char *const MAINTENANCE1_NAME = VK_KHR_MAINTENANCE_1_EXTENSION_NAME;
static VkInstance instance;
static VkDevice device;
static VkCommandPool pool;

static VkInstance make_instance(void)
{
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
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
static int lists(VkPhysicalDevice p)
{
    uint32_t count = 0;
    VkExtensionProperties properties[32];
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS && count <= 32);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, MAINTENANCE1_NAME)) {
            assert(properties[n].specVersion == VK_KHR_MAINTENANCE_1_SPEC_VERSION);
            return 1;
        }
    return 0;
}
static VkResult create(VkPhysicalDevice p, int extension, VkDevice *out)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = extension ? 1u : 0u, .ppEnabledExtensionNames = &MAINTENANCE1_NAME};
    return vkCreateDevice(p, &info, NULL, out);
}

static unsigned compiled;
static VkResult compile_program(void *context, const struct ps5vk_graphics_key *key, const void **out)
{ (void)context; (void)key; *out = &compiled; return VK_SUCCESS; }
static void release_program(void *context, const void *data) { (void)context; (void)data; }
static VkResult create_graphics(VkDevice d, const void *data, uint32_t primitive, void **out)
{ (void)d; (void)data; (void)primitive; *out = malloc(1); return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release_graphics(VkDevice d, void *state) { (void)d; free(state); }

static void open_device(int extension)
{
    instance = make_instance();
    assert(create(physical(instance), extension, &device) == VK_SUCCESS);
    assert(device->maintenance1_extension_enabled == (extension ? VK_TRUE : VK_FALSE));
    device->graphics_enabled = VK_TRUE;
    device->image_requirements = ps5vk_native_image_requirements;
    device->graphics_compiler_context = &compiled;
    device->graphics_acquire = compile_program;
    device->graphics_compiled_release = release_program;
    device->graphics_create = create_graphics;
    device->graphics_release = release_graphics;
    VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &info, NULL, &pool) == VK_SUCCESS);
}
static void close_device(void)
{
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
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

/* The DXVK viewport, and the same viewport's forms that stay invalid. */
static const VkViewport dxvk_viewport = {0.0f, 64.0f, 64.0f, -64.0f, 0.0f, 1.0f};
static const VkViewport zero_height = {0.0f, 64.0f, 64.0f, 0.0f, 0.0f, 1.0f};

static int set_viewport_accepted(const VkViewport *v)
{
    VkCommandBuffer c = begin();
    vkCmdSetViewport(c, 0, 1, v);
    const int accepted = c->state == PS5VK_RECORDING;
    if (accepted) assert(!memcmp(&c->viewports[0], v, sizeof(*v)));
    vkFreeCommandBuffers(device, pool, 1, &c);
    return accepted;
}

static VkResult static_pipeline(const VkViewport *v, VkPipeline *out)
{
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass pass = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(device, &pass_info, NULL, &pass) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    assert(vkCreatePipelineLayout(device, &layout_info, NULL, &layout) == VK_SUCCESS);
    static const uint32_t vs[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 0, 1, 0x6e69616d, 0};
    static const uint32_t fs[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 4, 1, 0x6e69616d, 0};
    VkShaderModule modules[2];
    for (unsigned n = 0; n < 2; ++n) {
        VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vs), .pCode = n ? fs : vs};
        assert(vkCreateShaderModule(device, &info, NULL, &modules[n]) == VK_SUCCESS);
    }
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
        .polygonMode = VK_POLYGON_MODE_FILL, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkRect2D scissor = {{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo viewports = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = v, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xfu};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly, .pViewportState = &viewports,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = layout, .renderPass = pass};
    *out = VK_NULL_HANDLE;
    VkResult rc = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, out);
    for (unsigned n = 0; n < 2; ++n) vkDestroyShaderModule(device, modules[n], NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    return rc;
}

int main(void)
{
    /* Negotiation. */
    platform_t09 = 0;
    VkInstance i = make_instance();
    VkPhysicalDevice p = physical(i);
    VkDevice d = VK_NULL_HANDLE;
    assert(!lists(p));
    assert(create(p, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
    platform_t09 = PS5VK_T09_FEATURE_MAINTENANCE1;
    i = make_instance();
    p = physical(i);
    assert(lists(p));
    /* No registry dependency: a plain 1.0 instance is enough. */
    assert(create(p, 1, &d) == VK_SUCCESS && d);
    assert(vkGetDeviceProcAddr(d, "vkTrimCommandPoolKHR"));
    assert(!vkGetDeviceProcAddr(d, "vkTrimCommandPool"));
    vkDestroyDevice(d, NULL);
    d = VK_NULL_HANDLE;
    assert(create(p, 0, &d) == VK_SUCCESS && d);
    assert(!vkGetDeviceProcAddr(d, "vkTrimCommandPoolKHR"));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
    /* 2D_ARRAY_COMPATIBLE is refused by the console's image format query for
     * every format, tiling and usage, which the extension permits. */
    static const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R32_UINT, VK_FORMAT_D32_SFLOAT};
    for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f)
        for (VkImageUsageFlags usage = 1; usage <= 0xffu; ++usage) {
            VkImageFormatProperties image_properties;
            assert(ps5vk_graphics_image_properties(formats[f], VK_IMAGE_TYPE_3D,
                VK_IMAGE_TILING_OPTIMAL, usage, VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT,
                1u << 30, &image_properties) == VK_ERROR_FORMAT_NOT_SUPPORTED);
        }
    /* The transfer format-feature bits on the format DXVK renders and reads
     * back, from the console's own table. */
    VkFormatProperties format;
    ps5vk_texture_format_properties(VK_FORMAT_R8G8B8A8_UNORM, &format);
    assert((format.optimalTilingFeatures &
            (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) ==
           (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT));

    /* Without the extension a negative height is refused everywhere. */
    open_device(0);
    assert(!set_viewport_accepted(&dxvk_viewport));
    assert(!set_viewport_accepted(&zero_height));
    VkPipeline pipeline = VK_NULL_HANDLE;
    assert(static_pipeline(&dxvk_viewport, &pipeline) != VK_SUCCESS && !pipeline);
    VkCommandPool trim_pool = pool;
    ++device->lifetime_errors;
    const unsigned errors = device->lifetime_errors;
    vkTrimCommandPoolKHR(device, trim_pool, 0);
    assert(device->lifetime_errors == errors + 1u);
    close_device();

    /* With it: accepted, stored unchanged, carried to the draw snapshot. */
    open_device(1);
    assert(set_viewport_accepted(&dxvk_viewport));
    assert(!set_viewport_accepted(&zero_height));
    const VkViewport nan_height = {0.0f, 64.0f, 64.0f, 0.0f / 0.0f, 0.0f, 1.0f};
    assert(!set_viewport_accepted(&nan_height));
    assert(static_pipeline(&dxvk_viewport, &pipeline) == VK_SUCCESS && pipeline);
    assert(pipeline->viewports[0].height == -64.0f && pipeline->viewports[0].y == 64.0f);
    vkDestroyPipeline(device, pipeline, NULL);
    const unsigned before = device->lifetime_errors;
    vkTrimCommandPoolKHR(device, pool, 0);
    assert(device->lifetime_errors == before);
    vkTrimCommandPoolKHR(device, pool, 1u);
    assert(device->lifetime_errors == before + 1u);
    close_device();
    puts("dxvk maintenance1 tests passed");
    return 0;
}

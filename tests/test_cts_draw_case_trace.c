/*
 * Host trace of the pinned upstream draw-parameter case sequence.
 *
 * The promotion's first hardware launches each discovered one more CTS
 * convention, so this fixture walks the whole sequence the pinned draw module
 * uses, in the order the module issues it, and pins every value that decides
 * whether the profile accepts the case:
 *
 *   1. the colour target (vktDrawBaseClass.cpp:59-66);
 *   2. the GENERAL render pass and framebuffer (vktDrawBaseClass.cpp:75-107);
 *   3. the pinned pipeline (vktDrawBaseClass.cpp:114-189): one vertex binding
 *      of sizeof(VertexElementData), the three attributes the pinned shader
 *      reads, TRIANGLE_STRIP and the default no-op depth bias;
 *   4. the pre-render transition, the clear in GENERAL and the memory barrier
 *      (vktDrawBaseClass.cpp:195-206);
 *   5. the pinned draw forms (vktDrawShaderDrawParametersTests.cpp:366-423);
 *   6. the readback path in full (vktDrawImageObjectUtil.cpp:392-443): the
 *      linear staging image, its two barriers, the GENERAL-to-GENERAL copy out
 *      of the colour attachment, the submit, and the host read of the staging
 *      rows through vkGetImageSubresourceLayout.
 *
 * The pipeline is created through the public entry point with the pinned
 * create-info, and the fixture's compiler callback asserts the key the profile
 * hands the compiler: topology, colour format, sample count, colour write mask,
 * and the exact binding/attribute layout. The recorded operations are then
 * inspected directly, so a profile that folded the signed baseVertex into
 * firstVertex, dropped the first instance, or accepted a multidraw command
 * would be caught here instead of on the console.
 *
 * It is a recording-layer trace plus the one execution path that needs no GPU:
 * the readback copy is host work in this profile, so it is submitted for real
 * and its result read from memory. The tiled colour surface is written through
 * the same 64KB_R_X offsets the hardware uses, so the assertion proves the copy
 * detiles instead of copying rows. Pipeline and draw execution stay covered by
 * tests/test_vk_graphics_pipeline.c, the emitter tests and the hardware session.
 */
#include "vk_internal.h"
#include "vk_command.h"
#include "vk_framebuffer.h"
#include "graphics_formats.h"
#include "graphics_program.h"
#include "physical_device_profile.h"
#include "color_detile.h"
#include "texture_layout.h"
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
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
                                 .max_allocation = 1u << 20,
                                 .queue_flags = VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU",
        .vendor_id = 0x1002u,
        .heap_size = 1u << 20,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

/* Pinned values, all from the module that runs the eight selected leaves. */
enum {
    WIDTH = 256,
    HEIGHT = 256,
    PINNED_VERTICES = 4,           /* vktDrawShaderDrawParametersTests.cpp:68 */
    PINNED_FIRST_VERTEX = 2,       /* :69 */
    PINNED_FIRST_INDEX = 11,       /* :71 */
    PINNED_VERTEX_OFFSET = 1,      /* :73 */
    PINNED_FIRST_INSTANCE = 2,     /* :416 */
    PINNED_INSTANCES = 3,          /* :75 MAX_INSTANCE_COUNT */
    PINNED_VERTEX_STRIDE = 36,     /* sizeof(VertexElementData), vktDrawBaseClass.cpp:116 */
    PINNED_INDEX_BYTES = 4 * (17 + 4), /* sizeof(uint32_t) * (NDX_SECOND_INDEX + NUM_VERTICES) */
    PINNED_INDIRECT_BYTES = 3 * 32     /* MAX_INDIRECT_DRAW_COUNT * 32 */
};

/* The three attributes the pinned shader reads (vktDrawBaseClass.cpp:120-127)
 * and the single binding that carries them (vktDrawBaseClass.cpp:114-118). */
static const VkVertexInputBindingDescription pinned_binding = {
    0u, PINNED_VERTEX_STRIDE, VK_VERTEX_INPUT_RATE_VERTEX};
static const VkVertexInputAttributeDescription pinned_attributes[3] = {
    {0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 0u},
    {1u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 16u},
    {2u, 0u, VK_FORMAT_R32_SINT, 32u}};

static unsigned compiled, compiled_released, linked, linked_released;
static uint32_t linked_primitive = 0xffffffffu;

/* Injected compiler: the only place the profile's compiler input is visible. */
static VkResult compile_program(void *context, const struct ps5vk_graphics_key *key, const void **out)
{
    assert(context == &compiled && key && out);
    assert(key->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    assert(key->color_format[0] == VK_FORMAT_R8G8B8A8_UNORM);
    assert(key->samples == VK_SAMPLE_COUNT_1_BIT);
    assert(key->color_write_mask[0] == 0xfu);
    assert(key->blend_enable[0] == VK_FALSE);
    assert(key->descriptor_set_count == 0u && key->push_constant_size == 0u);
    assert(key->vertex_binding_count == 1u && key->vertex_attribute_count == 3u);
    assert(memcmp(key->vertex_bindings, &pinned_binding, sizeof(pinned_binding)) == 0);
    assert(memcmp(key->vertex_attributes, pinned_attributes, sizeof(pinned_attributes)) == 0);
    /* The profile's own vertex-format metadata must carry every pinned
     * attribute format: the pinned shader reads all three. */
    for (unsigned i = 0; i < 3; ++i) {
        struct ps5vk_vertex_format meta = ps5vk_vertex_format_info(pinned_attributes[i].format);
        assert(meta.components == (i == 2 ? 1u : 4u));
        assert(meta.bytes == (i == 2 ? 4u : 16u));
    }
    ++compiled;
    *out = (const void *)&compiled;
    return VK_SUCCESS;
}
static void release_program(void *context, const void *data)
{ assert(context == &compiled && data == (const void *)&compiled); ++compiled_released; }
static VkResult create_graphics(VkDevice d, const void *data, uint32_t primitive_type, void **out)
{
    (void)d; assert(data == (const void *)&compiled && out);
    linked_primitive = primitive_type; ++linked;
    *out = malloc(1);
    return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void release_graphics(VkDevice d, void *state)
{ (void)d; assert(state); ++linked_released; free(state); }

static VkDevice device;
static VkCommandPool pool;

static VkImage make_image(VkImageTiling tiling, VkImageUsageFlags usage, VkDeviceMemory *memory_out)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM,
                              .extent = {WIDTH, HEIGHT, 1},
                              .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = tiling, .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage image = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = requirements.size,
                                       .memoryTypeIndex = 0u};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS);
    *memory_out = memory;
    return image;
}

/* Host-visible buffer whose span the fixture can inspect, like the module's
 * own Buffer::createAndAlloc(..., MemoryRequirement::HostVisible). */
static VkBuffer make_buffer(VkBufferUsageFlags usage, VkDeviceSize size, void **address,
                            VkDeviceMemory *memory_out)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer = VK_NULL_HANDLE;
    assert(vkCreateBuffer(device, &info, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = requirements.size, .memoryTypeIndex = 0u};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    VkDeviceSize bytes = 0;
    assert(ps5vk_buffer_span(device, buffer, 0, VK_WHOLE_SIZE, address, &bytes) == VK_SUCCESS);
    assert(bytes >= size);
    *memory_out = memory;
    return buffer;
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
    VkCommandBuffer command = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command, &begin_info) == VK_SUCCESS);
    return command;
}

int main(void)
{
    VkInstance instance;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&instance_info, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    assert(vkCreateDevice(physical, &device_info, NULL, &device) == VK_SUCCESS);
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

    /* 1. The colour target the pinned draw module creates. */
    VkDeviceMemory target_memory = VK_NULL_HANDLE;
    VkImage target = make_image(VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT, &target_memory);
    assert(ps5vk_colour_transfer_image(target));

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = target,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view = VK_NULL_HANDLE;
    assert(vkCreateImageView(device, &view_info, NULL, &view) == VK_SUCCESS);

    /* 2. The GENERAL render pass and framebuffer the pinned draw module uses. */
    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass pass = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(device, &pass_info, NULL, &pass) == VK_SUCCESS);
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view,
        .width = WIDTH, .height = HEIGHT, .layers = 1};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    assert(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer) == VK_SUCCESS);

    /* 3. The pinned pipeline. Every state below is the module's default; the
     *    rasterizer really does enable depth bias, with all three factors and
     *    the clamp at zero. */
    uint32_t vertex_module[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 0, 1, 0x6e69616d, 0};
    uint32_t fragment_module[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 4, 1, 0x6e69616d, 0};
    VkShaderModule modules[2];
    for (unsigned i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo module_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vertex_module), .pCode = i ? fragment_module : vertex_module};
        assert(vkCreateShaderModule(device, &module_info, NULL, &modules[i]) == VK_SUCCESS);
    }
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    assert(vkCreatePipelineLayout(device, &layout_info, NULL, &layout) == VK_SUCCESS);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &pinned_binding,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = pinned_attributes};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE, .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE, .depthBiasEnable = VK_TRUE,
        .depthBiasConstantFactor = 0.0f, .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f, .lineWidth = 1.0f};
    const VkSampleMask sample_mask = 0xffffffffu;
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .pSampleMask = &sample_mask};
    VkViewport viewport = {0.0f, 0.0f, (float)WIDTH, (float)HEIGHT, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {WIDTH, HEIGHT}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xfu};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    /* Vulkan ignores this state when the pipeline contains no tessellation
     * stages. The CTS helper supplies this otherwise-unused pointer. */
    VkPipelineTessellationStateCreateInfo tessellation = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .layout = layout, .renderPass = pass, .subpass = 0u,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &input_assembly,
        .pTessellationState = &tessellation, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_state, .pColorBlendState = &blend};
    VkPipeline pipeline = VK_NULL_HANDLE;
    assert(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS);
    assert(pipeline->graphics == VK_TRUE && compiled == 1u);
    assert(linked == 1u && linked_primitive == PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP);

    /* 4. The pre-render sequence: UNDEFINED to GENERAL for a transfer write,
     *    the clear in GENERAL, an upload in GENERAL, then a memory barrier into
     *    the colour-attachment stages. The eight selected leaves clear the
     *    target (vktDrawBaseClass.cpp:203); the upload is the destination gate
     *    this slice also implemented, and both must stay accepted. */
    VkCommandBuffer command = begin();
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_general = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target, .subresourceRange = range};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
    assert(command->state == PS5VK_RECORDING);
    VkClearColorValue clear_value = {0};
    clear_value.float32[3] = 1.0f;
    vkCmdClearColorImage(command, target, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1, &range);
    assert(command->state == PS5VK_RECORDING);
    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0}, .imageExtent = {WIDTH, HEIGHT, 1}};
    void *source_address = NULL;
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
    VkBuffer source = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  (VkDeviceSize)WIDTH * HEIGHT * 4, &source_address, &source_memory);
    assert(source_address);
    vkCmdCopyBufferToImage(command, source, target, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    assert(command->state == PS5VK_RECORDING);
    VkMemoryBarrier memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &memory, 0, NULL, 0, NULL);
    assert(command->state == PS5VK_RECORDING);

    /* 5. The pinned draw forms (vktDrawShaderDrawParametersTests.cpp:366-423).
     *    Vertex, index and indirect buffers are created the way the module
     *    creates them, and the indirect buffer carries the module's own
     *    command words. */
    void *vertex_address = NULL, *index_address = NULL, *indirect_address = NULL;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE, index_memory = VK_NULL_HANDLE;
    VkDeviceMemory indirect_memory = VK_NULL_HANDLE;
    VkBuffer vertices = make_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        (VkDeviceSize)PINNED_VERTEX_STRIDE * (17 + PINNED_VERTICES + 2), &vertex_address,
        &vertex_memory);
    VkBuffer indices = make_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, PINNED_INDEX_BYTES,
                                   &index_address, &index_memory);
    VkBuffer indirect = make_buffer(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, PINNED_INDIRECT_BYTES,
                                    &indirect_address, &indirect_memory);
    assert(vertex_address && index_address && indirect_address);
    {
        uint32_t *index_words = index_address;
        for (unsigned i = 0; i < PINNED_VERTICES; ++i)
            index_words[PINNED_FIRST_INDEX + i] = PINNED_FIRST_VERTEX + i - PINNED_VERTEX_OFFSET;
        /* indexCount, instanceCount, firstVertex, firstInstance (vktDrawShaderDrawParametersTests.cpp:392-396) */
        const VkDrawIndirectCommand direct_words[3] = {
            {PINNED_VERTICES, 1u, PINNED_FIRST_VERTEX, PINNED_FIRST_INSTANCE},
            {PINNED_VERTICES, 1u, 9u, PINNED_FIRST_INSTANCE},
            {PINNED_VERTICES, 1u, PINNED_FIRST_VERTEX, PINNED_FIRST_INSTANCE}};
        /* indexCount, instanceCount, firstIndex, vertexOffset, firstInstance (:382-387) */
        const VkDrawIndexedIndirectCommand indexed_words[3] = {
            {PINNED_VERTICES, 1u, PINNED_FIRST_INDEX, PINNED_VERTEX_OFFSET, PINNED_FIRST_INSTANCE},
            {PINNED_VERTICES, 1u, 17u, 4u, PINNED_FIRST_INSTANCE},
            {PINNED_VERTICES, 1u, PINNED_FIRST_INDEX, PINNED_VERTEX_OFFSET, PINNED_FIRST_INSTANCE}};
        memcpy(indirect_address, direct_words, sizeof(direct_words));

        VkRenderPassBeginInfo pass_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass, .framebuffer = framebuffer,
            .renderArea = {{0, 0}, {WIDTH, HEIGHT}}};
        vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        assert(command->state == PS5VK_RECORDING && command->render_pass == pass);

        const VkDeviceSize no_offset = 0;
        vkCmdBindVertexBuffers(command, 0, 1, &vertices, &no_offset);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindIndexBuffer(command, indices, 0, VK_INDEX_TYPE_UINT32);
        assert(command->state == PS5VK_RECORDING && command->graphics_pipeline == pipeline);
        assert(command->indices.buffer == indices && command->indices.type == VK_INDEX_TYPE_UINT32);

        /* base_vertex.draw / base_vertex.draw_indexed. */
        uint32_t before = command->operation_count;
        vkCmdDraw(command, PINNED_VERTICES, 1u, PINNED_FIRST_VERTEX, PINNED_FIRST_INSTANCE);
        assert(command->state == PS5VK_RECORDING && command->operation_count == before + 1);
        {
            const struct ps5vk_operation *op = &command->operations[before];
            assert(op->type == PS5VK_DRAW && op->vertex_count == PINNED_VERTICES);
            assert(op->instance_count == 1u && op->first_vertex == PINNED_FIRST_VERTEX);
            assert(op->first_instance == PINNED_FIRST_INSTANCE);
        }
        before = command->operation_count;
        vkCmdDrawIndexed(command, PINNED_VERTICES, 1u, PINNED_FIRST_INDEX, PINNED_VERTEX_OFFSET,
                         PINNED_FIRST_INSTANCE);
        assert(command->state == PS5VK_RECORDING && command->operation_count == before + 1);
        {
            const struct ps5vk_operation *op = &command->operations[before];
            /* The signed base vertex has its own field: it must never be folded
             * into firstVertex, which is a non-indexed parameter. */
            assert(op->type == PS5VK_DRAW_INDEXED && op->index_count == PINNED_VERTICES);
            assert(op->first_index == PINNED_FIRST_INDEX && op->vertex_offset == PINNED_VERTEX_OFFSET);
            assert(op->first_vertex == 0u && op->first_instance == PINNED_FIRST_INSTANCE);
        }

        /* base_instance.draw / base_instance.draw_indexed, three instances. */
        before = command->operation_count;
        vkCmdDraw(command, PINNED_VERTICES, PINNED_INSTANCES, PINNED_FIRST_VERTEX, PINNED_FIRST_INSTANCE);
        assert(command->state == PS5VK_RECORDING && command->operation_count == before + 1);
        assert(command->operations[before].instance_count == PINNED_INSTANCES);
        before = command->operation_count;
        vkCmdDrawIndexed(command, PINNED_VERTICES, PINNED_INSTANCES, PINNED_FIRST_INDEX,
                         PINNED_VERTEX_OFFSET, PINNED_FIRST_INSTANCE);
        assert(command->state == PS5VK_RECORDING && command->operation_count == before + 1);
        assert(command->operations[before].instance_count == PINNED_INSTANCES);

        /* base_vertex.draw_indirect / base_instance.draw_indirect and their
         * indexed forms: one draw per command, which is what the pinned
         * multiDrawIndirect=false contract requires. */
        before = command->operation_count;
        vkCmdDrawIndirect(command, indirect, 0, 1u, sizeof(VkDrawIndirectCommand));
        assert(command->state == PS5VK_RECORDING && command->operation_count == before + 1);
        assert(command->operations[before].type == PS5VK_DRAW_INDIRECT);
        before = command->operation_count;
        vkCmdDrawIndexedIndirect(command, indirect, 0, 1u, sizeof(VkDrawIndexedIndirectCommand));
        assert(command->state == PS5VK_RECORDING && command->operation_count == before + 1);
        assert(command->operations[before].type == PS5VK_DRAW_INDEXED_INDIRECT);
        (void)indexed_words;

        vkCmdEndRenderPass(command);
        assert(command->state == PS5VK_RECORDING && !command->render_pass);
    }
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);

    /* 6. Two indirect commands in one buffer must stay refused while
     *    multiDrawIndirect is false: the four draw_index leaves are
     *    NotSupported for exactly this reason. */
    {
        VkCommandBuffer rejected = begin();
        VkRenderPassBeginInfo pass_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass, .framebuffer = framebuffer,
            .renderArea = {{0, 0}, {WIDTH, HEIGHT}}};
        vkCmdBeginRenderPass(rejected, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        const VkDeviceSize no_offset = 0;
        vkCmdBindVertexBuffers(rejected, 0, 1, &vertices, &no_offset);
        vkCmdBindPipeline(rejected, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDrawIndirect(rejected, indirect, 0, 2u, sizeof(VkDrawIndirectCommand));
        assert(rejected->state != PS5VK_RECORDING);
    }

    /* 7. The pinned readback, executed for real. The staging image is linear
     *    memory this profile describes honestly, so the module's sequence -
     *    transition, GENERAL-to-GENERAL copy, host-read barrier - runs to
     *    completion here and the host reads the staging rows afterwards. */
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkImage staging = make_image(VK_IMAGE_TILING_LINEAR,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT, &staging_memory);
    assert(ps5vk_linear_staging_image(staging));
    assert(ps5vk_colour_transfer_image(target));
    assert(!ps5vk_pure_transfer_image(staging));

    /* The colour target's content, written the way the GPU writes it: through
     * the 64KB_R_X tiled offsets the detile below has to invert, not row by
     * row. */
    void *target_address = NULL;
    VkDeviceSize target_bytes = 0;
    assert(ps5vk_image_span(device, target, &target_address, &target_bytes) == VK_SUCCESS);
    const size_t tiled_surface = ps5vk_color_64k_rx_surface_size(4u, WIDTH, HEIGHT);
    assert(tiled_surface != (size_t)-1 && target_bytes >= tiled_surface);
    const uint32_t pinned_words[3] = {0x11223344u, 0xaabbccddu, 0x00ff7f3fu};
    const uint32_t pinned_x[3] = {0u, 7u, WIDTH - 1u};
    const uint32_t pinned_y[3] = {0u, 5u, HEIGHT - 1u};
    for (unsigned i = 0; i < 3; ++i) {
        const size_t tiled_offset =
            ps5vk_rgba8_64k_rx_offset(pinned_x[i], pinned_y[i], WIDTH);
        assert(tiled_offset + 4u <= target_bytes);
        memcpy((unsigned char *)target_address + tiled_offset, &pinned_words[i], 4);
    }
    /* The producing submission committed GENERAL: that is the pinned case's own
     * transition (vktDrawBaseClass.cpp:197-203). This fixture has no graphics
     * backend, so the committed state the readback depends on is injected here
     * exactly as the module leaves it. */
    target->layout = VK_IMAGE_LAYOUT_GENERAL;

    VkCommandBuffer readback = begin();
    VkImageSubresourceRange staging_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier staging_in = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = staging, .subresourceRange = staging_range};
    vkCmdPipelineBarrier(readback, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &staging_in);
    assert(readback->state == PS5VK_RECORDING);
    VkImageCopy readback_region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffset = {0, 0, 0},
        .extent = {WIDTH, HEIGHT, 1}};
    vkCmdCopyImage(readback, target, VK_IMAGE_LAYOUT_GENERAL, staging,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &readback_region);
    assert(readback->state == PS5VK_RECORDING);
    VkImageMemoryBarrier staging_out = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = staging, .subresourceRange = staging_range};
    vkCmdPipelineBarrier(readback, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &staging_out);
    assert(readback->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(readback) == VK_SUCCESS);
    {
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        assert(vkCreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS);
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .commandBufferCount = 1, .pCommandBuffers = &readback};
        assert(vkQueueSubmit(&device->queue, 1, &submit, fence) == VK_SUCCESS);
        assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000ull) == VK_SUCCESS);
        vkDestroyFence(device, fence, NULL);
    }

    /* vkGetImageSubresourceLayout must describe this image honestly, because
     * that is how the module addresses the rows it reads. */
    VkImageSubresource staging_subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout staging_layout = {0};
    vkGetImageSubresourceLayout(device, staging, &staging_subresource, &staging_layout);
    struct ps5vk_texture_layout expected_layout = {0};
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT,
        &expected_layout) == 0);
    assert(staging_layout.offset == 0);
    assert(staging_layout.rowPitch == expected_layout.row_pitch);
    assert(staging_layout.depthPitch == expected_layout.bytes);
    assert(staging_layout.arrayPitch == expected_layout.bytes);
    assert(staging_layout.size == expected_layout.bytes);
    /* A tiled image still reports nothing: no layout is fabricated for it. */
    VkSubresourceLayout tiled_layout = {0};
    vkGetImageSubresourceLayout(device, target, &staging_subresource, &tiled_layout);
    assert(!tiled_layout.offset && !tiled_layout.rowPitch && !tiled_layout.size);

    /* Every pinned sample must come back at its linear row position, and the
     * middle one must not coincide with its tiled offset - that is what makes
     * this a detile rather than a row copy. */
    void *staging_address = NULL;
    VkDeviceSize staging_bytes = 0;
    assert(ps5vk_image_span(device, staging, &staging_address, &staging_bytes) == VK_SUCCESS);
    assert(staging_bytes >= staging_layout.size);
    for (unsigned i = 0; i < 3; ++i) {
        const size_t linear_offset = (size_t)pinned_y[i] * staging_layout.rowPitch +
            (size_t)pinned_x[i] * 4u;
        uint32_t word = 0;
        assert(linear_offset + 4u <= staging_bytes);
        memcpy(&word, (unsigned char *)staging_address + linear_offset, 4);
        assert(word == pinned_words[i]);
    }
    assert(ps5vk_rgba8_64k_rx_offset(pinned_x[1], pinned_y[1], WIDTH) !=
           (size_t)pinned_y[1] * staging_layout.rowPitch + (size_t)pinned_x[1] * 4u);

    /* The pinned scissor case's own sequence over this same colour target:
     * clear it through the transfer destination outside the pass, then read
     * the rendered result back from it. Each call is asserted separately so a
     * refusal names the command that produced it. */
    {
        VkBufferCreateInfo host_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = (VkDeviceSize)WIDTH * HEIGHT * 4u,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkBuffer host_buffer = VK_NULL_HANDLE;
        assert(vkCreateBuffer(device, &host_info, NULL, &host_buffer) == VK_SUCCESS);
        VkMemoryRequirements need; vkGetBufferMemoryRequirements(device, host_buffer, &need);
        VkMemoryAllocateInfo host_alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = need.size, .memoryTypeIndex = 0};
        VkDeviceMemory host_memory = VK_NULL_HANDLE;
        assert(vkAllocateMemory(device, &host_alloc, NULL, &host_memory) == VK_SUCCESS);
        assert(vkBindBufferMemory(device, host_buffer, host_memory, 0) == VK_SUCCESS);

        VkImageMemoryBarrier acquire = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = target,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageMemoryBarrier to_attachment = acquire;
        to_attachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_attachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_attachment.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        VkClearColorValue clear_colour = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
        VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkCommandBuffer cleared = begin();
        vkCmdPipelineBarrier(cleared, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &acquire);
        assert(cleared->state == PS5VK_RECORDING);
        vkCmdClearColorImage(cleared, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear_colour, 1, &whole);
        assert(cleared->state == PS5VK_RECORDING);
        vkCmdPipelineBarrier(cleared, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_attachment);
        assert(cleared->state == PS5VK_RECORDING);
        assert(vkEndCommandBuffer(cleared) == VK_SUCCESS);

        /* The postlude transition and the separately submitted readback. */
        VkImageMemoryBarrier to_source = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = target,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkCommandBuffer handed = begin();
        vkCmdPipelineBarrier(handed, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_source);
        assert(handed->state == PS5VK_RECORDING);
        assert(vkEndCommandBuffer(handed) == VK_SUCCESS);

        VkBufferImageCopy to_host = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {WIDTH, HEIGHT, 1}};
        VkBufferMemoryBarrier host_read = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = host_buffer, .size = VK_WHOLE_SIZE};
        VkCommandBuffer fetched = begin();
        vkCmdCopyImageToBuffer(fetched, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               host_buffer, 1, &to_host);
        assert(fetched->state == PS5VK_RECORDING);
        vkCmdPipelineBarrier(fetched, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host_read, 0, NULL);
        assert(fetched->state == PS5VK_RECORDING);
        assert(vkEndCommandBuffer(fetched) == VK_SUCCESS);

        vkDestroyBuffer(device, host_buffer, NULL);
        vkFreeMemory(device, host_memory, NULL);
    }

    /* Negatives: only the measured shapes may be recorded. Each call must leave
     * the command buffer invalid instead of appending work. */
    VkDeviceMemory pure_transfer_memory = VK_NULL_HANDLE;
    VkImage pure_transfer = make_image(VK_IMAGE_TILING_OPTIMAL,
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                       &pure_transfer_memory);
    assert(ps5vk_pure_transfer_image(pure_transfer));
    {
        VkCommandBuffer rejected = begin();
        vkCmdPipelineBarrier(rejected, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &staging_in);
        assert(rejected->state == PS5VK_RECORDING);
        VkImageCopy partial = readback_region;
        partial.extent.width = WIDTH - 1u;
        vkCmdCopyImage(rejected, target, VK_IMAGE_LAYOUT_GENERAL, staging,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &partial);
        assert(rejected->state != PS5VK_RECORDING);

        /* A pure transfer image is not a colour-attachment readback source. */
        rejected = begin();
        vkCmdCopyImage(rejected, pure_transfer, VK_IMAGE_LAYOUT_GENERAL, staging,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &readback_region);
        assert(rejected->state != PS5VK_RECORDING);

        /* The staging role is never a transfer source. */
        rejected = begin();
        vkCmdCopyImage(rejected, staging, VK_IMAGE_LAYOUT_GENERAL, target,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &readback_region);
        assert(rejected->state != PS5VK_RECORDING);

        /* Only the two measured barriers exist for this role. */
        rejected = begin();
        VkImageMemoryBarrier wrong_layout = staging_in;
        wrong_layout.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(rejected, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &wrong_layout);
        assert(rejected->state != PS5VK_RECORDING);
        rejected = begin();
        VkImageMemoryBarrier wrong_access = staging_out;
        wrong_access.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(rejected, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &wrong_access);
        assert(rejected->state != PS5VK_RECORDING);
    }

    /* Teardown through the public entry points, so the backend release path
     * runs for the pipeline exactly once and the fixture is leak-clean. */
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyShaderModule(device, modules[0], NULL);
    vkDestroyShaderModule(device, modules[1], NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, target, NULL);
    vkFreeMemory(device, target_memory, NULL);
    vkDestroyBuffer(device, source, NULL);
    vkFreeMemory(device, source_memory, NULL);
    vkDestroyBuffer(device, vertices, NULL);
    vkFreeMemory(device, vertex_memory, NULL);
    vkDestroyBuffer(device, indices, NULL);
    vkFreeMemory(device, index_memory, NULL);
    vkDestroyBuffer(device, indirect, NULL);
    vkFreeMemory(device, indirect_memory, NULL);
    vkDestroyImage(device, staging, NULL);
    vkFreeMemory(device, staging_memory, NULL);
    vkDestroyImage(device, pure_transfer, NULL);
    vkFreeMemory(device, pure_transfer_memory, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    assert(compiled == 1u && compiled_released == 1u && linked == 1u && linked_released == 1u);
    puts("Pinned draw-case trace: target, render pass, pinned pipeline, clear, upload, the six pinned draw forms, and the linear staging readback (barriers, GENERAL copy, submit, host read through vkGetImageSubresourceLayout) are accepted, with the tiled surface detiled into the pinned samples");
    return 0;
}

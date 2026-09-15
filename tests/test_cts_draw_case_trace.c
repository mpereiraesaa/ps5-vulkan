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
 *   6. the readback staging image, which the profile still refuses
 *      (vktDrawImageObjectUtil.cpp:392-443).
 *
 * The pipeline is created through the public entry point with the pinned
 * create-info, and the fixture's compiler callback asserts the key the profile
 * hands the compiler: topology, colour format, sample count, colour write mask,
 * and the exact binding/attribute layout. The recorded operations are then
 * inspected directly, so a profile that folded the signed baseVertex into
 * firstVertex, dropped the first instance, or accepted a multidraw command
 * would be caught here instead of on the console.
 *
 * It is a recording-layer trace: no GPU work, no submission and no readback are
 * performed, so the pipeline/draw/readback execution gates remain covered by
 * tests/test_vk_graphics_pipeline.c, the emitter tests and the hardware session.
 */
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
    assert(key->color_format == VK_FORMAT_R8G8B8A8_UNORM);
    assert(key->samples == VK_SAMPLE_COUNT_1_BIT);
    assert(key->color_write_mask == 0xfu);
    assert(key->blend_enable == VK_FALSE);
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

    /* 7. The one remaining boundary: the pinned readback stages through a
     *    VK_IMAGE_TILING_LINEAR image, which this profile refuses because every
     *    image it accepts is tiled optimal. When that role is implemented this
     *    assertion becomes the creation of the staging image plus the
     *    GENERAL-to-GENERAL image copy the module issues. */
    VkImageCreateInfo staging_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {WIDTH, HEIGHT, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage staging = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &staging_info, NULL, &staging) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !staging);

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
    vkDestroyCommandPool(device, pool, NULL);
    assert(compiled == 1u && compiled_released == 1u && linked == 1u && linked_released == 1u);
    puts("Pinned draw-case trace: target, render pass, pinned pipeline, clear, upload, the six pinned draw forms and the render pass are accepted; the linear readback staging image is the one boundary left");
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Descriptor-set capacity witnesses: public-SDK payloads (no private headers
 * or symbols), one per build variant.
 *
 * DESCRIPTOR_WITNESS_CAPACITY (default): 1024 distinct 1x1 RGBA8 images.
 *   Compute reads 1023 of them through one SAMPLED_IMAGE[1023] binding and
 *   writes each texel to a storage buffer in the SAME set, so that set holds
 *   exactly 1024 descriptors. A fragment shader reads all 1024 through a
 *   SAMPLED_IMAGE[1024] set: pixel k of a 32x32 target is image k's texel.
 *   Every index is a constant (no dynamic-indexing feature is used).
 *
 * DESCRIPTOR_WITNESS_DYNAMIC: dynamic offsets and live set contents.
 *   Compute: four dispatches, each with its own UNIFORM_BUFFER_DYNAMIC and
 *   STORAGE_BUFFER_DYNAMIC offset, copy one uvec4 each. The uniform binding
 *   is then rewritten to a second buffer; the recorded command buffer must be
 *   refused on resubmission, and a newly recorded one must read the new
 *   buffer. Graphics: four one-pixel draws with their own dynamic uniform
 *   offset into a 4x1 target, then the same after the rewrite into a second
 *   target.
 *
 * These are capability measurements: they use more descriptors than the
 * limits the device reports today. Every submission waits on a bounded fence
 * and every result is exact.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "descriptor_capacity_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(DESCRIPTOR_WITNESS_BISECT)
/* The bisect variant uses only pipeline creation. */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#endif

#if defined(DESCRIPTOR_WITNESS_BISECT)
#define MARK "DESCRIPTOR_BISECT_WITNESS"
#elif defined(DESCRIPTOR_WITNESS_DYNAMIC)
#define MARK "DESCRIPTOR_DYNAMIC_WITNESS"
#else
#define MARK "DESCRIPTOR_CAPACITY_WITNESS"
#endif

static const uint64_t fence_timeout = UINT64_C(300000000);
static const uint32_t sentinel = UINT32_C(0xcdcdcdcd);

static uint32_t digest_bytes(const void *p, size_t bytes)
{
    const uint8_t *b = p;
    uint32_t digest = UINT32_C(2166136261);
    for (size_t i = 0; i < bytes; ++i) digest = (digest ^ b[i]) * UINT32_C(16777619);
    return digest;
}

struct buffer { VkBuffer buffer; VkDeviceMemory memory; uint8_t *host; };

static VkResult make_buffer(VkDevice device, VkDeviceSize bytes, VkBufferUsageFlags usage,
                            struct buffer *out)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult rc = vkCreateBuffer(device, &info, NULL, &out->buffer);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out->buffer, &req);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0};
    rc = vkAllocateMemory(device, &allocation, NULL, &out->memory);
    if (rc != VK_SUCCESS) return rc;
    rc = vkBindBufferMemory(device, out->buffer, out->memory, 0);
    if (rc != VK_SUCCESS) return rc;
    return vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, (void **)&out->host);
}

static void destroy_buffer(VkDevice device, struct buffer *buffer)
{
    if (buffer->buffer) vkDestroyBuffer(device, buffer->buffer, NULL);
    if (buffer->memory) vkFreeMemory(device, buffer->memory, NULL);
    memset(buffer, 0, sizeof(*buffer));
}

static VkResult sync_memory(VkDevice device, VkDeviceMemory memory, int invalidate)
{
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .size = VK_WHOLE_SIZE};
    return invalidate ? vkInvalidateMappedMemoryRanges(device, 1, &range) :
                        vkFlushMappedMemoryRanges(device, 1, &range);
}

static VkResult make_image(VkDevice device, uint32_t width, uint32_t height,
                           VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory,
                           VkImageView *view)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult rc = vkCreateImage(device, &info, NULL, image);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, *image, &req);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0};
    rc = vkAllocateMemory(device, &allocation, NULL, memory);
    if (rc != VK_SUCCESS) return rc;
    rc = vkBindImageMemory(device, *image, *memory, 0);
    if (rc != VK_SUCCESS) return rc;
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(device, &view_info, NULL, view);
}

static void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
    VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void host_barrier(VkCommandBuffer command, VkPipelineStageFlags src_stage,
                         VkAccessFlags src_access)
{
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, src_stage, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                         0, NULL, 0, NULL);
}

/* Target -> readback buffer, recorded after the render pass. */
static void record_readback(VkCommandBuffer command, VkImage target, uint32_t width,
                            uint32_t height, VkBuffer readback)
{
    image_barrier(command, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {width, height, 1}};
    vkCmdCopyImageToBuffer(command, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback, 1, &region);
    VkBufferMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback, .offset = 0, .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &to_host, 0, NULL);
}

struct graphics {
    VkRenderPass pass;
    VkPipeline pipeline;
    VkShaderModule vertex, fragment;
};

static VkResult make_graphics(VkDevice device, VkPipelineLayout layout,
    const uint32_t *fragment_code, size_t fragment_bytes, uint32_t width, uint32_t height,
    VkBool32 dynamic_scissor, struct graphics *g)
{
    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(descriptor_capacity_vert_spirv), .pCode = descriptor_capacity_vert_spirv};
    VkResult rc = vkCreateShaderModule(device, &shader_info, NULL, &g->vertex);
    if (rc != VK_SUCCESS) return rc;
    shader_info.codeSize = fragment_bytes; shader_info.pCode = fragment_code;
    rc = vkCreateShaderModule(device, &shader_info, NULL, &g->fragment);
    if (rc != VK_SUCCESS) return rc;
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
    rc = vkCreateRenderPass(device, &pass_info, NULL, &g->pass);
    if (rc != VK_SUCCESS) return rc;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g->vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g->fragment, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo viewport_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1,
        .pScissors = dynamic_scissor ? NULL : &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    const VkDynamicState dynamic_state = VK_DYNAMIC_STATE_SCISSOR;
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 1, .pDynamicStates = &dynamic_state};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly, .pViewportState = &viewport_info,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .pDynamicState = dynamic_scissor ? &dynamic : NULL,
        .layout = layout, .renderPass = g->pass};
    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                     &g->pipeline);
}

static void destroy_graphics(VkDevice device, struct graphics *g)
{
    if (g->pipeline) vkDestroyPipeline(device, g->pipeline, NULL);
    if (g->pass) vkDestroyRenderPass(device, g->pass, NULL);
    if (g->fragment) vkDestroyShaderModule(device, g->fragment, NULL);
    if (g->vertex) vkDestroyShaderModule(device, g->vertex, NULL);
}

static VkResult submit_wait(VkDevice device, VkQueue queue, VkFence fence,
                            VkCommandBuffer command, VkBool32 *pending)
{
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    VkResult rc = vkQueueSubmit(queue, 1, &submit, fence);
    if (rc != VK_SUCCESS) return rc;
    *pending = VK_TRUE;
    rc = vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout);
    if (rc != VK_SUCCESS) return rc;
    *pending = VK_FALSE;
    return vkResetFences(device, 1, &fence);
}

#if defined(DESCRIPTOR_WITNESS_BISECT)
#include <stdlib.h>
/* Heap headroom, then compute pipelines over sets of 128..1023 sampled
 * images (plus the output buffer), each compiled on its own, no dispatch.
 * The last STEP line names the largest size that compiled. */
static const uint32_t *const bisect_code[] = {descriptor_bisect_0_spirv, descriptor_bisect_1_spirv,
    descriptor_bisect_2_spirv, descriptor_bisect_3_spirv, descriptor_bisect_4_spirv};
static const size_t bisect_bytes[] = {sizeof(descriptor_bisect_0_spirv),
    sizeof(descriptor_bisect_1_spirv), sizeof(descriptor_bisect_2_spirv),
    sizeof(descriptor_bisect_3_spirv), sizeof(descriptor_bisect_4_spirv)};
static const uint32_t bisect_images[] = {128, 256, 512, 768, 1023};
static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    for (unsigned mib = 16; mib <= 1024; mib *= 2) {
        void *probe = malloc((size_t)mib << 20);
        ps5log_printf(PS5LOG_MARK, MARK "_HEAP mib=%u ok=%d", mib, probe != NULL);
        if (!probe) break;
        memset(probe, 0x5a, (size_t)mib << 20);
        free(probe);
    }
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    ps5log_printf(PS5LOG_MARK, MARK "_START sizes=128,256,512,768,1023");
    for (unsigned n = 0; n < 5; ++n) {
        VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, bisect_images[n], VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = bindings};
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        TRY(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
        VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &set_layout};
        TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
        VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = bisect_bytes[n], .pCode = bisect_code[n]};
        TRY(vkCreateShaderModule(device, &shader_info, NULL, &module));
        ps5log_printf(PS5LOG_MARK, MARK "_STEP compile images=%u", bisect_images[n]);
        VkComputePipelineCreateInfo compute_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
            .layout = layout};
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_info, NULL, &pipeline);
        ps5log_printf(PS5LOG_MARK, MARK "_STEP compiled images=%u result=%d", bisect_images[n], (int)result);
        if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
        vkDestroyShaderModule(device, module, NULL);
        vkDestroyPipelineLayout(device, layout, NULL);
        vkDestroyDescriptorSetLayout(device, set_layout, NULL);
        if (result != VK_SUCCESS) { failed = "vkCreateComputePipelines"; goto cleanup; }
    }
cleanup:
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS) ps5log_printf(PS5LOG_MARK, MARK "_RETIRED resources=clean");
    else ps5log_printf(PS5LOG_ERR, MARK "_FAILURE call=%s result=%d retirement=attempted",
                       failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
#undef TRY
}
#elif !defined(DESCRIPTOR_WITNESS_DYNAMIC)
enum { IMAGES = 1024, COMPUTE_IMAGES = IMAGES - 1, SIDE = 32, BATCH = 16 };

static uint32_t texel(uint32_t k)
{
    return (k & 255u) | (((k >> 8) | 0x40u) << 8) | (((k * 37u) & 255u) << 16) |
           ((255u - (k & 255u)) << 24);
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
    static VkImage images[IMAGES];
    static VkDeviceMemory memories[IMAGES];
    static VkImageView views[IMAGES];
    static VkDescriptorImageInfo infos[IMAGES];
    VkImage target = VK_NULL_HANDLE; VkDeviceMemory target_memory = VK_NULL_HANDLE;
    VkImageView target_view = VK_NULL_HANDLE;
    struct buffer upload = {0}, output = {0}, readback = {0};
    VkDescriptorSetLayout compute_set_layout = VK_NULL_HANDLE, pixel_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE};
    VkShaderModule compute_module = VK_NULL_HANDLE;
    VkPipelineLayout compute_layout = VK_NULL_HANDLE, pixel_layout = VK_NULL_HANDLE;
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    struct graphics g = {0};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 pending = VK_FALSE;

    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    ps5log_printf(PS5LOG_MARK, MARK "_START images=%u compute_set=%u pixel_set=%u",
                  IMAGES, COMPUTE_IMAGES + 1, IMAGES);

    VkCommandPoolCreateInfo pool_create = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
    TRY(vkCreateCommandPool(device, &pool_create, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    /* 1024 distinct one-texel images, uploaded in bounded batches. */
    TRY(make_buffer(device, IMAGES * 4u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload));
    for (uint32_t k = 0; k < IMAGES; ++k) memcpy(upload.host + 4u * k, &(uint32_t){texel(k)}, 4);
    TRY(sync_memory(device, upload.memory, 0));
    for (uint32_t k = 0; k < IMAGES; ++k) {
        TRY(make_image(device, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       &images[k], &memories[k], &views[k]));
        infos[k] = (VkDescriptorImageInfo){.imageView = views[k],
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    for (uint32_t first = 0; first < IMAGES; first += BATCH) {
        TRY(vkBeginCommandBuffer(command, &begin));
        for (uint32_t k = first; k < first + BATCH; ++k) {
            image_barrier(command, images[k], VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region = {.bufferOffset = 4u * k,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {1, 1, 1}};
            vkCmdCopyBufferToImage(command, upload.buffer, images[k],
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            image_barrier(command, images[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        TRY(vkEndCommandBuffer(command));
        TRY(submit_wait(device, queue, fence, command, &pending));
        TRY(vkResetCommandBuffer(command, 0));
    }
    ps5log_printf(PS5LOG_MARK, MARK "_UPLOADED images=%u", IMAGES);

    /* Compute set: SAMPLED_IMAGE[1023] + the output buffer = 1024 descriptors. */
    TRY(make_buffer(device, IMAGES * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &output));
    memset(output.host, 0xcd, IMAGES * 4u);
    TRY(sync_memory(device, output.memory, 0));
    ps5log_printf(PS5LOG_MARK, MARK "_STEP output");
    VkDescriptorSetLayoutBinding compute_bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, COMPUTE_IMAGES, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = compute_bindings};
    TRY(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &compute_set_layout));
    VkDescriptorSetLayoutBinding pixel_binding =
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    set_layout_info.bindingCount = 1; set_layout_info.pBindings = &pixel_binding;
    TRY(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &pixel_set_layout));
    ps5log_printf(PS5LOG_MARK, MARK "_STEP layouts");
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, COMPUTE_IMAGES + IMAGES},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = pool_sizes};
    TRY(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetLayout set_layouts[2] = {compute_set_layout, pixel_set_layout};
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 2, .pSetLayouts = set_layouts};
    TRY(vkAllocateDescriptorSets(device, &set_info, sets));
    ps5log_printf(PS5LOG_MARK, MARK "_STEP sets");
    VkDescriptorBufferInfo output_info = {output.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 0,
         .descriptorCount = COMPUTE_IMAGES, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .pImageInfo = infos},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &output_info},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[1], .dstBinding = 0,
         .descriptorCount = IMAGES, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .pImageInfo = infos}};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    ps5log_printf(PS5LOG_MARK, MARK "_STEP written");

    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(descriptor_capacity_comp_spirv), .pCode = descriptor_capacity_comp_spirv};
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &compute_module));
    ps5log_printf(PS5LOG_MARK, MARK "_STEP compute_module");
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &compute_set_layout};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &compute_layout));
    layout_info.pSetLayouts = &pixel_set_layout;
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &pixel_layout));
    VkComputePipelineCreateInfo compute_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = compute_module, .pName = "main"},
        .layout = compute_layout};
    TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_info, NULL, &compute_pipeline));
    ps5log_printf(PS5LOG_MARK, MARK "_STEP compute_pipeline");
    TRY(make_graphics(device, pixel_layout, descriptor_capacity_frag_spirv,
                      sizeof(descriptor_capacity_frag_spirv), SIDE, SIDE, VK_FALSE, &g));
    ps5log_printf(PS5LOG_MARK, MARK "_STEP graphics_pipeline");
    TRY(make_image(device, SIDE, SIDE, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &target, &target_memory, &target_view));
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = g.pass, .attachmentCount = 1, .pAttachments = &target_view,
        .width = SIDE, .height = SIDE, .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));
    ps5log_printf(PS5LOG_MARK, MARK "_STEP framebuffer");
    TRY(make_buffer(device, SIDE * SIDE * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback));
    memset(readback.host, 0xcd, SIDE * SIDE * 4u);
    TRY(sync_memory(device, readback.memory, 0));

    TRY(vkBeginCommandBuffer(command, &begin));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout, 0, 1,
                            &sets[0], 0, NULL);
    vkCmdDispatch(command, 1, 1, 1);
    host_barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    TRY(vkEndCommandBuffer(command));
    ps5log_printf(PS5LOG_MARK, MARK "_STEP dispatch_recorded");
    TRY(submit_wait(device, queue, fence, command, &pending));
    TRY(vkResetCommandBuffer(command, 0));

    ps5log_printf(PS5LOG_MARK, MARK "_STEP dispatch_done");
    TRY(vkBeginCommandBuffer(command, &begin));
    VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}};
    VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g.pass, .framebuffer = framebuffer, .renderArea = {{0, 0}, {SIDE, SIDE}},
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pixel_layout, 0, 1,
                            &sets[1], 0, NULL);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    record_readback(command, target, SIDE, SIDE, readback.buffer);
    TRY(vkEndCommandBuffer(command));
    TRY(submit_wait(device, queue, fence, command, &pending));
    TRY(sync_memory(device, output.memory, 1));
    TRY(sync_memory(device, readback.memory, 1));

    uint32_t compute_mismatches = 0, pixel_mismatches = 0, first_compute = UINT32_MAX,
        first_pixel = UINT32_MAX;
    for (uint32_t k = 0; k < COMPUTE_IMAGES; ++k) {
        uint32_t value; memcpy(&value, output.host + 4u * k, 4);
        if (value != texel(k)) { ++compute_mismatches; if (first_compute == UINT32_MAX) first_compute = k; }
    }
    uint32_t guard; memcpy(&guard, output.host + 4u * COMPUTE_IMAGES, 4);
    for (uint32_t k = 0; k < IMAGES; ++k) {
        uint32_t value; memcpy(&value, readback.host + 4u * k, 4);
        if (value != texel(k)) { ++pixel_mismatches; if (first_pixel == UINT32_MAX) first_pixel = k; }
    }
    ps5log_printf(PS5LOG_MARK, MARK "_RESULT compute_mismatches=%u first_compute=%d"
        " pixel_mismatches=%u first_pixel=%d guard=%08x digest_compute=%08x digest_pixels=%08x",
        compute_mismatches, (int)first_compute, pixel_mismatches, (int)first_pixel, guard,
        digest_bytes(output.host, COMPUTE_IMAGES * 4u), digest_bytes(readback.host, IMAGES * 4u));
    REQUIRE(!compute_mismatches, "compute: 1023 sampled images in a 1024-descriptor set");
    REQUIRE(guard == sentinel, "compute: output guard untouched");
    REQUIRE(!pixel_mismatches, "graphics: 1024 sampled images in one set");

cleanup:
    if (pending && device && vkDeviceWaitIdle(device) != VK_SUCCESS) {
        ps5log_printf(PS5LOG_ERR, MARK "_FAILURE call=%s result=%d retirement=pending",
                      failed ? failed : "idle", (int)result);
        return 1;
    }
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command) vkFreeCommandBuffers(device, pool, 1, &command);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    destroy_graphics(device, &g);
    if (compute_pipeline) vkDestroyPipeline(device, compute_pipeline, NULL);
    if (pixel_layout) vkDestroyPipelineLayout(device, pixel_layout, NULL);
    if (compute_layout) vkDestroyPipelineLayout(device, compute_layout, NULL);
    if (compute_module) vkDestroyShaderModule(device, compute_module, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (pixel_set_layout) vkDestroyDescriptorSetLayout(device, pixel_set_layout, NULL);
    if (compute_set_layout) vkDestroyDescriptorSetLayout(device, compute_set_layout, NULL);
    if (target_view) vkDestroyImageView(device, target_view, NULL);
    if (target) vkDestroyImage(device, target, NULL);
    if (target_memory) vkFreeMemory(device, target_memory, NULL);
    for (uint32_t k = 0; k < IMAGES; ++k) {
        if (views[k]) vkDestroyImageView(device, views[k], NULL);
        if (images[k]) vkDestroyImage(device, images[k], NULL);
        if (memories[k]) vkFreeMemory(device, memories[k], NULL);
    }
    destroy_buffer(device, &readback);
    destroy_buffer(device, &output);
    destroy_buffer(device, &upload);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS) ps5log_printf(PS5LOG_MARK, MARK "_RETIRED resources=clean");
    else ps5log_printf(PS5LOG_ERR, MARK "_FAILURE call=%s result=%d retirement=attempted",
                       failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
#undef TRY
#undef REQUIRE
}
#else
enum { DRAWS = 4, STRIDE = 256, SLOTS = 2 * DRAWS };

/* uvec4 at offset i * STRIDE of source buffer s (0: A, 1: B). */
static uint32_t source_word(uint32_t s, uint32_t i, uint32_t c)
{ return (s ? 0xb0000000u : 0xa0000000u) | (i << 8) | (c << 4) | 0x5u; }

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
    struct buffer sources[2] = {{0}}, output = {0}, readback[2] = {{0}};
    VkImage targets[2] = {VK_NULL_HANDLE}; VkDeviceMemory target_memory[2] = {VK_NULL_HANDLE};
    VkImageView target_views[2] = {VK_NULL_HANDLE};
    VkFramebuffer framebuffers[2] = {VK_NULL_HANDLE};
    VkDescriptorSetLayout compute_set_layout = VK_NULL_HANDLE, pixel_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE};
    VkShaderModule compute_module = VK_NULL_HANDLE;
    VkPipelineLayout compute_layout = VK_NULL_HANDLE, pixel_layout = VK_NULL_HANDLE;
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    struct graphics g = {0};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[4] = {VK_NULL_HANDLE};
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 pending = VK_FALSE;

    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    ps5log_printf(PS5LOG_MARK, MARK "_START draws=%u stride=%u", DRAWS, STRIDE);

    for (uint32_t s = 0; s < 2; ++s) {
        TRY(make_buffer(device, DRAWS * STRIDE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &sources[s]));
        memset(sources[s].host, 0, DRAWS * STRIDE);
        for (uint32_t i = 0; i < DRAWS; ++i)
            for (uint32_t c = 0; c < 4; ++c)
                memcpy(sources[s].host + i * STRIDE + 4 * c, &(uint32_t){source_word(s, i, c)}, 4);
        TRY(sync_memory(device, sources[s].memory, 0));
        TRY(make_image(device, DRAWS, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &targets[s], &target_memory[s],
                       &target_views[s]));
        TRY(make_buffer(device, DRAWS * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback[s]));
        memset(readback[s].host, 0xcd, DRAWS * 4u);
        TRY(sync_memory(device, readback[s].memory, 0));
    }
    TRY(make_buffer(device, SLOTS * STRIDE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &output));
    memset(output.host, 0xcd, SLOTS * STRIDE);
    TRY(sync_memory(device, output.memory, 0));

    VkDescriptorSetLayoutBinding compute_bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = compute_bindings};
    TRY(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &compute_set_layout));
    VkDescriptorSetLayoutBinding pixel_binding =
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    set_layout_info.bindingCount = 1; set_layout_info.pBindings = &pixel_binding;
    TRY(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &pixel_set_layout));
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = pool_sizes};
    TRY(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetLayout set_layouts[2] = {compute_set_layout, pixel_set_layout};
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 2, .pSetLayouts = set_layouts};
    TRY(vkAllocateDescriptorSets(device, &set_info, sets));
    VkDescriptorBufferInfo uniform_info = {sources[0].buffer, 0, 16};
    VkDescriptorBufferInfo output_info = {output.buffer, 0, 16};
    VkWriteDescriptorSet writes[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .pBufferInfo = &uniform_info},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
         .pBufferInfo = &output_info},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[1], .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .pBufferInfo = &uniform_info}};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(descriptor_dynamic_comp_spirv), .pCode = descriptor_dynamic_comp_spirv};
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &compute_module));
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &compute_set_layout};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &compute_layout));
    layout_info.pSetLayouts = &pixel_set_layout;
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &pixel_layout));
    VkComputePipelineCreateInfo compute_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = compute_module, .pName = "main"},
        .layout = compute_layout};
    TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_info, NULL, &compute_pipeline));
    TRY(make_graphics(device, pixel_layout, descriptor_dynamic_frag_spirv,
                      sizeof(descriptor_dynamic_frag_spirv), DRAWS, 1, VK_TRUE, &g));
    for (uint32_t s = 0; s < 2; ++s) {
        VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = g.pass, .attachmentCount = 1, .pAttachments = &target_views[s],
            .width = DRAWS, .height = 1, .layers = 1};
        TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffers[s]));
    }

    VkCommandPoolCreateInfo pool_create = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0};
    TRY(vkCreateCommandPool(device, &pool_create, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 4};
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    /* Pass s (0: before the rewrite, 1: after): commands[2s] computes into
     * output slots 4s..4s+3, commands[2s+1] draws into target s. */
    VkResult resubmit = VK_SUCCESS;
    for (uint32_t s = 0; s < 2; ++s) {
        if (s) {
            /* Rewrite the uniform binding of both sets to buffer B. The
             * command buffers recorded against the old contents become
             * invalid; a resubmission must be refused. */
            uniform_info.buffer = sources[1].buffer;
            writes[0].pBufferInfo = &uniform_info; writes[2].pBufferInfo = &uniform_info;
            VkWriteDescriptorSet rewrite[2] = {writes[0], writes[2]};
            vkUpdateDescriptorSets(device, 2, rewrite, 0, NULL);
            VkSubmitInfo stale = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1, .pCommandBuffers = &commands[0]};
            resubmit = vkQueueSubmit(queue, 1, &stale, VK_NULL_HANDLE);
            ps5log_printf(PS5LOG_MARK, MARK "_STALE_RESUBMIT result=%d", (int)resubmit);
            REQUIRE(resubmit != VK_SUCCESS, "recorded command buffer invalidated by the rewrite");
        }
        VkCommandBuffer compute = commands[2 * s], draw = commands[2 * s + 1];
        TRY(vkBeginCommandBuffer(compute, &begin));
        vkCmdBindPipeline(compute, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
        for (uint32_t i = 0; i < DRAWS; ++i) {
            const uint32_t offsets[2] = {i * STRIDE, (s * DRAWS + i) * STRIDE};
            vkCmdBindDescriptorSets(compute, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout, 0, 1,
                                    &sets[0], 2, offsets);
            vkCmdDispatch(compute, 1, 1, 1);
        }
        host_barrier(compute, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        TRY(vkEndCommandBuffer(compute));
        TRY(submit_wait(device, queue, fence, compute, &pending));

        TRY(vkBeginCommandBuffer(draw, &begin));
        VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}};
        VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = g.pass, .framebuffer = framebuffers[s],
            .renderArea = {{0, 0}, {DRAWS, 1}}, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(draw, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(draw, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
        for (uint32_t i = 0; i < DRAWS; ++i) {
            const uint32_t offset = i * STRIDE;
            VkRect2D pixel = {{(int32_t)i, 0}, {1, 1}};
            vkCmdSetScissor(draw, 0, 1, &pixel);
            vkCmdBindDescriptorSets(draw, VK_PIPELINE_BIND_POINT_GRAPHICS, pixel_layout, 0, 1,
                                    &sets[1], 1, &offset);
            vkCmdDraw(draw, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(draw);
        record_readback(draw, targets[s], DRAWS, 1, readback[s].buffer);
        TRY(vkEndCommandBuffer(draw));
        TRY(submit_wait(device, queue, fence, draw, &pending));
    }
    TRY(sync_memory(device, output.memory, 1));
    for (uint32_t s = 0; s < 2; ++s) TRY(sync_memory(device, readback[s].memory, 1));

    uint32_t compute_mismatches = 0, pixel_mismatches = 0, guards = 0;
    for (uint32_t slot = 0; slot < SLOTS; ++slot) {
        const uint32_t s = slot / DRAWS, i = slot % DRAWS;
        for (uint32_t c = 0; c < STRIDE / 4; ++c) {
            uint32_t value; memcpy(&value, output.host + slot * STRIDE + 4 * c, 4);
            if (c < 4) compute_mismatches += value != source_word(s, i, c);
            else guards += value != sentinel;
        }
    }
    for (uint32_t s = 0; s < 2; ++s)
        for (uint32_t i = 0; i < DRAWS; ++i) {
            uint32_t value; memcpy(&value, readback[s].host + 4 * i, 4);
            pixel_mismatches += value != source_word(s, i, 0);
        }
    ps5log_printf(PS5LOG_MARK, MARK "_RESULT compute_mismatches=%u pixel_mismatches=%u"
        " guards=%u stale_resubmit=%d digest_compute=%08x digest_pixels=%08x%08x",
        compute_mismatches, pixel_mismatches, guards, (int)resubmit,
        digest_bytes(output.host, SLOTS * STRIDE), digest_bytes(readback[0].host, DRAWS * 4u),
        digest_bytes(readback[1].host, DRAWS * 4u));
    REQUIRE(!compute_mismatches && !guards, "compute: per-dispatch dynamic offsets, live rewrite");
    REQUIRE(!pixel_mismatches, "graphics: per-draw dynamic offsets, live rewrite");

cleanup:
    if (pending && device && vkDeviceWaitIdle(device) != VK_SUCCESS) {
        ps5log_printf(PS5LOG_ERR, MARK "_FAILURE call=%s result=%d retirement=pending",
                      failed ? failed : "idle", (int)result);
        return 1;
    }
    if (fence) vkDestroyFence(device, fence, NULL);
    if (commands[0]) vkFreeCommandBuffers(device, pool, 4, commands);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    for (uint32_t s = 0; s < 2; ++s)
        if (framebuffers[s]) vkDestroyFramebuffer(device, framebuffers[s], NULL);
    destroy_graphics(device, &g);
    if (compute_pipeline) vkDestroyPipeline(device, compute_pipeline, NULL);
    if (pixel_layout) vkDestroyPipelineLayout(device, pixel_layout, NULL);
    if (compute_layout) vkDestroyPipelineLayout(device, compute_layout, NULL);
    if (compute_module) vkDestroyShaderModule(device, compute_module, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (pixel_set_layout) vkDestroyDescriptorSetLayout(device, pixel_set_layout, NULL);
    if (compute_set_layout) vkDestroyDescriptorSetLayout(device, compute_set_layout, NULL);
    for (uint32_t s = 0; s < 2; ++s) {
        if (target_views[s]) vkDestroyImageView(device, target_views[s], NULL);
        if (targets[s]) vkDestroyImage(device, targets[s], NULL);
        if (target_memory[s]) vkFreeMemory(device, target_memory[s], NULL);
        destroy_buffer(device, &readback[s]);
        destroy_buffer(device, &sources[s]);
    }
    destroy_buffer(device, &output);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS) ps5log_printf(PS5LOG_MARK, MARK "_RETIRED resources=clean");
    else ps5log_printf(PS5LOG_ERR, MARK "_FAILURE call=%s result=%d retirement=attempted",
                       failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
#undef TRY
#undef REQUIRE
}
#endif

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
    ps5log_close(failed ? "descriptor-witness-failed" : "descriptor-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

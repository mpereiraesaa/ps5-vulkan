/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Separate-sampler and texel-buffer witness: a public-SDK payload (no private
 * headers or symbols) for the descriptor forms DXVK's DXBC translation binds
 * for every D3D11 sample, Buffer<> SRV and typed UAV buffer.
 *
 * Reporting (measurement build): R32_UINT, R8G8B8A8_UNORM and
 * R32G32B32A32_SFLOAT report STORAGE_TEXEL_BUFFER, R32_SINT does not, and a
 * storage-texel view of R32_SINT is refused.
 *
 * Compute: one dispatch of eight invocations reads an 8x4 RGBA8 texture
 * through a SAMPLER plus a SAMPLED_IMAGE combined in the shader (sample_l and
 * resinfo), a Buffer<uint> SRV through a UNIFORM_TEXEL_BUFFER (ld), and writes
 * three RWBuffer<> UAVs through STORAGE_TEXEL_BUFFERs of R32_UINT,
 * R8G8B8A8_UNORM and R32G32B32A32_SFLOAT (store_uav_typed).
 *
 * Graphics: one full-screen draw into an 8x4 RGBA8 target with DXVK's
 * pixel-shader view set - SAMPLER at binding 0, SAMPLED_IMAGE at 1,
 * UNIFORM_TEXEL_BUFFER at 2 - samples each pixel's own texel.
 *
 * Every result is exact (NEAREST at texel centres, unorm8 round trips and
 * small integers as floats) and compared byte for byte with the host oracle.
 * Every submission waits on a bounded fence.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "dxvk_separate_sampler_shaders.h"
#include "dxvk_separate_sampler_data.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { W = SEPARATE_SAMPLER_WIDTH, H = SEPARATE_SAMPLER_HEIGHT,
       TEXTURE_BYTES = W * H * 4, U32_BYTES = W * 4, RGBA8_BYTES = W * 4,
       RGBA32F_BYTES = W * 16, PIXEL_BYTES = W * H * 4, STORAGE_VIEWS = 3 };
static const uint64_t fence_timeout = UINT64_C(300000000);
static const VkFormat storage_formats[STORAGE_VIEWS] = {
    VK_FORMAT_R32_UINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT};
static const VkDeviceSize storage_bytes[STORAGE_VIEWS] = {
    U32_BYTES, RGBA8_BYTES, RGBA32F_BYTES};

static uint32_t digest_bytes(const uint8_t *p, size_t bytes)
{
    uint32_t digest = UINT32_C(2166136261);
    for (size_t i = 0; i < bytes; ++i) digest = (digest ^ p[i]) * UINT32_C(16777619);
    return digest;
}

static uint32_t mismatches(const uint8_t *a, const uint8_t *b, size_t bytes)
{
    uint32_t count = 0;
    for (size_t i = 0; i < bytes; ++i) count += a[i] != b[i];
    return count;
}

struct buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *host;
};

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

static VkResult make_image(VkDevice device, VkImageUsageFlags usage, VkImage *image,
                           VkDeviceMemory *memory)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
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
    return vkBindImageMemory(device, *image, *memory, 0);
}

static VkResult make_view(VkDevice device, VkImage image, VkImageView *out)
{
    VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(device, &info, NULL, out);
}

static VkResult make_texel_view(VkDevice device, VkBuffer buffer, VkFormat format,
                                VkDeviceSize range, VkBufferView *out)
{
    VkBufferViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = buffer, .format = format, .offset = 0, .range = range};
    return vkCreateBufferView(device, &info, NULL, out);
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
    VkImage texture = VK_NULL_HANDLE, target = VK_NULL_HANDLE;
    VkDeviceMemory texture_memory = VK_NULL_HANDLE, target_memory = VK_NULL_HANDLE;
    VkImageView texture_view = VK_NULL_HANDLE, target_view = VK_NULL_HANDLE;
    struct buffer upload = {0}, fetch = {0}, readback = {0};
    struct buffer storage[STORAGE_VIEWS] = {{0}};
    VkBufferView fetch_view = VK_NULL_HANDLE, refused_view = VK_NULL_HANDLE;
    VkBufferView storage_views[STORAGE_VIEWS] = {VK_NULL_HANDLE};
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout compute_set_layout = VK_NULL_HANDLE, pixel_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet compute_set = VK_NULL_HANDLE, pixel_set = VK_NULL_HANDLE;
    VkShaderModule compute_module = VK_NULL_HANDLE, vertex = VK_NULL_HANDLE,
        fragment = VK_NULL_HANDLE;
    VkPipelineLayout compute_layout = VK_NULL_HANDLE, pixel_layout = VK_NULL_HANDLE;
    VkPipeline compute_pipeline = VK_NULL_HANDLE, pixel_pipeline = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[3] = {VK_NULL_HANDLE};
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 pending = VK_FALSE;

    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");

    /* Reporting: the measurement build's storage-texel rows, and a control. */
    VkFormatProperties properties[STORAGE_VIEWS + 1];
    for (unsigned n = 0; n < STORAGE_VIEWS; ++n)
        vkGetPhysicalDeviceFormatProperties(physical, storage_formats[n], &properties[n]);
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32_SINT, &properties[STORAGE_VIEWS]);
    ps5log_printf(PS5LOG_MARK,
        "DXVK_SEPARATE_SAMPLER_WITNESS_START width=%u height=%u r32ui=%08x rgba8=%08x"
        " rgba32f=%08x r32i=%08x", W, H,
        (unsigned)properties[0].bufferFeatures, (unsigned)properties[1].bufferFeatures,
        (unsigned)properties[2].bufferFeatures, (unsigned)properties[3].bufferFeatures);
    for (unsigned n = 0; n < STORAGE_VIEWS; ++n)
        REQUIRE(properties[n].bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
                "storage-texel row reported");
    REQUIRE(!(properties[STORAGE_VIEWS].bufferFeatures &
              VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT), "R32_SINT not storage-texel");
    REQUIRE(properties[0].bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
            "R32_UINT uniform texel reported");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");

    /* Resources. */
    TRY(make_image(device, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   &texture, &texture_memory));
    TRY(make_view(device, texture, &texture_view));
    TRY(make_image(device, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   &target, &target_memory));
    TRY(make_view(device, target, &target_view));
    TRY(make_buffer(device, TEXTURE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload));
    memcpy(upload.host, separate_sampler_texture, TEXTURE_BYTES);
    TRY(sync_memory(device, upload.memory, 0));
    TRY(make_buffer(device, U32_BYTES, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &fetch));
    memcpy(fetch.host, separate_sampler_fetch, U32_BYTES);
    TRY(sync_memory(device, fetch.memory, 0));
    TRY(make_texel_view(device, fetch.buffer, VK_FORMAT_R32_UINT, U32_BYTES, &fetch_view));
    for (unsigned n = 0; n < STORAGE_VIEWS; ++n) {
        /* DXVK's UAV buffer usage: storage texel beside storage buffer. */
        TRY(make_buffer(device, storage_bytes[n], VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &storage[n]));
        memset(storage[n].host, 0xcd, storage_bytes[n]);
        TRY(sync_memory(device, storage[n].memory, 0));
        TRY(make_texel_view(device, storage[n].buffer, storage_formats[n], storage_bytes[n],
                            &storage_views[n]));
    }
    const VkResult refused = make_texel_view(device, storage[0].buffer, VK_FORMAT_R32_SINT,
                                             U32_BYTES, &refused_view);
    ps5log_printf(PS5LOG_MARK,
        "DXVK_SEPARATE_SAMPLER_WITNESS_REFUSAL view=r32i_storage_texel result=%d", (int)refused);
    REQUIRE(refused != VK_SUCCESS, "R32_SINT storage-texel view refused");
    TRY(make_buffer(device, PIXEL_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback));
    memset(readback.host, 0xcd, PIXEL_BYTES);
    TRY(sync_memory(device, readback.memory, 0));

    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    TRY(vkCreateSampler(device, &sampler_info, NULL, &sampler));

    /* Descriptor sets in DXVK's in-set order: SAMPLER, SAMPLED_IMAGE, then the
     * texel buffers. */
    VkDescriptorSetLayoutBinding bindings[6] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 6, .pBindings = bindings};
    TRY(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &compute_set_layout));
    for (unsigned n = 0; n < 3; ++n) bindings[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    set_layout_info.bindingCount = 3;
    TRY(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &pixel_set_layout));
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 3}};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 4, .pPoolSizes = pool_sizes};
    TRY(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetLayout set_layouts[2] = {compute_set_layout, pixel_set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 2, .pSetLayouts = set_layouts};
    TRY(vkAllocateDescriptorSets(device, &set_info, sets));
    compute_set = sets[0]; pixel_set = sets[1];
    VkDescriptorImageInfo sampler_write = {.sampler = sampler};
    VkDescriptorImageInfo image_write = {.imageView = texture_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (unsigned n = 0; n < 2; ++n) {
        VkWriteDescriptorSet writes[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[n],
             .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &sampler_write},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[n],
             .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &image_write},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[n],
             .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
             .pTexelBufferView = &fetch_view}};
        vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    }
    VkWriteDescriptorSet storage_write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = compute_set, .dstBinding = 3, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    for (unsigned n = 0; n < STORAGE_VIEWS; ++n) {
        storage_write.dstBinding = 3 + n;
        storage_write.pTexelBufferView = &storage_views[n];
        vkUpdateDescriptorSets(device, 1, &storage_write, 0, NULL);
    }

    /* Pipelines. */
    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(separate_sampler_comp_spirv), .pCode = separate_sampler_comp_spirv};
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &compute_module));
    shader_info.codeSize = sizeof(separate_sampler_vert_spirv);
    shader_info.pCode = separate_sampler_vert_spirv;
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &vertex));
    shader_info.codeSize = sizeof(separate_sampler_frag_spirv);
    shader_info.pCode = separate_sampler_frag_spirv;
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &fragment));
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &compute_set_layout};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &compute_layout));
    layout_info.pSetLayouts = &pixel_set_layout;
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &pixel_layout));
    VkComputePipelineCreateInfo compute_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = compute_module,
                  .pName = "main"},
        .layout = compute_layout};
    TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_info, NULL,
                                 &compute_pipeline));

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
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &target_view,
        .width = W, .height = H, .layers = 1};
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0.0f, 0.0f, W, H, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo viewport_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
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
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly, .pViewportState = &viewport_info,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = pixel_layout, .renderPass = pass};
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                  &pixel_pipeline));

    VkCommandPoolCreateInfo pool_create = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0};
    TRY(vkCreateCommandPool(device, &pool_create, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3};
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    /* 0: upload the texture. */
    TRY(vkBeginCommandBuffer(commands[0], &begin));
    image_barrier(commands[0], texture, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy upload_region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageExtent = {W, H, 1}};
    vkCmdCopyBufferToImage(commands[0], upload.buffer, texture,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);
    image_barrier(commands[0], texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    TRY(vkEndCommandBuffer(commands[0]));
    /* 1: the compute dispatch. */
    TRY(vkBeginCommandBuffer(commands[1], &begin));
    vkCmdBindPipeline(commands[1], VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
    vkCmdBindDescriptorSets(commands[1], VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout, 0, 1,
                            &compute_set, 0, NULL);
    vkCmdDispatch(commands[1], 1, 1, 1);
    host_barrier(commands[1], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    TRY(vkEndCommandBuffer(commands[1]));
    /* 2: the draw and its readback. */
    TRY(vkBeginCommandBuffer(commands[2], &begin));
    VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}};
    VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer, .renderArea = {{0, 0}, {W, H}},
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(commands[2], &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands[2], VK_PIPELINE_BIND_POINT_GRAPHICS, pixel_pipeline);
    vkCmdBindDescriptorSets(commands[2], VK_PIPELINE_BIND_POINT_GRAPHICS, pixel_layout, 0, 1,
                            &pixel_set, 0, NULL);
    vkCmdDraw(commands[2], 3, 1, 0, 0);
    vkCmdEndRenderPass(commands[2]);
    image_barrier(commands[2], target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy readback_region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                         .imageExtent = {W, H, 1}};
    vkCmdCopyImageToBuffer(commands[2], target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &readback_region);
    host_barrier(commands[2], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    TRY(vkEndCommandBuffer(commands[2]));
    for (unsigned n = 0; n < 3; ++n) {
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &commands[n]};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        pending = VK_TRUE;
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
        pending = VK_FALSE;
        TRY(vkResetFences(device, 1, &fence));
    }
    for (unsigned n = 0; n < STORAGE_VIEWS; ++n) TRY(sync_memory(device, storage[n].memory, 1));
    TRY(sync_memory(device, readback.memory, 1));

    /* Oracles. */
    const uint32_t u32_mismatches = mismatches(storage[0].host,
        (const uint8_t *)separate_sampler_expected_u32, U32_BYTES);
    const uint32_t rgba8_mismatches = mismatches(storage[1].host,
        separate_sampler_expected_rgba8, RGBA8_BYTES);
    const uint32_t rgba32f_mismatches = mismatches(storage[2].host,
        separate_sampler_expected_rgba32f, RGBA32F_BYTES);
    const uint32_t pixel_mismatches = mismatches(readback.host,
        separate_sampler_expected_pixels, PIXEL_BYTES);
    ps5log_printf(PS5LOG_MARK,
        "DXVK_SEPARATE_SAMPLER_WITNESS_RESULT u32_mismatches=%u rgba8_mismatches=%u"
        " rgba32f_mismatches=%u pixel_mismatches=%u digest_u32=%08x digest_rgba8=%08x"
        " digest_rgba32f=%08x digest_pixels=%08x",
        u32_mismatches, rgba8_mismatches, rgba32f_mismatches, pixel_mismatches,
        digest_bytes(storage[0].host, U32_BYTES), digest_bytes(storage[1].host, RGBA8_BYTES),
        digest_bytes(storage[2].host, RGBA32F_BYTES), digest_bytes(readback.host, PIXEL_BYTES));
    REQUIRE(!u32_mismatches, "compute: separate sample, texel fetch and R32 store");
    REQUIRE(!rgba8_mismatches, "compute: RGBA8 storage texel store");
    REQUIRE(!rgba32f_mismatches, "compute: RGBA32F storage texel store");
    REQUIRE(!pixel_mismatches, "graphics: separate sample and texel fetch");

cleanup:
    if (pending && device && vkDeviceWaitIdle(device) != VK_SUCCESS) {
        ps5log_printf(PS5LOG_ERR,
            "DXVK_SEPARATE_SAMPLER_WITNESS_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "idle", (int)result);
        return 1;
    }
    if (fence) vkDestroyFence(device, fence, NULL);
    if (commands[0]) vkFreeCommandBuffers(device, pool, 3, commands);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (pixel_pipeline) vkDestroyPipeline(device, pixel_pipeline, NULL);
    if (compute_pipeline) vkDestroyPipeline(device, compute_pipeline, NULL);
    if (pixel_layout) vkDestroyPipelineLayout(device, pixel_layout, NULL);
    if (compute_layout) vkDestroyPipelineLayout(device, compute_layout, NULL);
    if (fragment) vkDestroyShaderModule(device, fragment, NULL);
    if (vertex) vkDestroyShaderModule(device, vertex, NULL);
    if (compute_module) vkDestroyShaderModule(device, compute_module, NULL);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (pixel_set_layout) vkDestroyDescriptorSetLayout(device, pixel_set_layout, NULL);
    if (compute_set_layout) vkDestroyDescriptorSetLayout(device, compute_set_layout, NULL);
    if (sampler) vkDestroySampler(device, sampler, NULL);
    if (refused_view) vkDestroyBufferView(device, refused_view, NULL);
    for (unsigned n = 0; n < STORAGE_VIEWS; ++n)
        if (storage_views[n]) vkDestroyBufferView(device, storage_views[n], NULL);
    if (fetch_view) vkDestroyBufferView(device, fetch_view, NULL);
    for (unsigned n = 0; n < STORAGE_VIEWS; ++n) destroy_buffer(device, &storage[n]);
    destroy_buffer(device, &readback);
    destroy_buffer(device, &fetch);
    destroy_buffer(device, &upload);
    if (target_view) vkDestroyImageView(device, target_view, NULL);
    if (texture_view) vkDestroyImageView(device, texture_view, NULL);
    if (target) vkDestroyImage(device, target, NULL);
    if (target_memory) vkFreeMemory(device, target_memory, NULL);
    if (texture) vkDestroyImage(device, texture, NULL);
    if (texture_memory) vkFreeMemory(device, texture_memory, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "DXVK_SEPARATE_SAMPLER_WITNESS_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR,
            "DXVK_SEPARATE_SAMPLER_WITNESS_FAILURE call=%s result=%d retirement=attempted",
            failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
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
    ps5log_close(failed ? "dxvk-separate-sampler-witness-failed" :
                          "dxvk-separate-sampler-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

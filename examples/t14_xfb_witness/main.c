/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DXVK262-T14 transform feedback witness: a public-SDK payload that measures
 * VK_EXT_transform_feedback end to end on this device, through the capture
 * shape the pinned DXVK emits (a pass-through geometry stage over points,
 * rasterizer discard, four counter slots).
 *
 * Every captured vertex k carries v = (k, 2k+1, k & 255, 7) and its swizzle,
 * so a record names the vertex that produced it: the oracle checks content
 * AND order, and the bytes after the last expected record must keep their
 * sentinel. Cases, each one bounded submission:
 *   inactive  a capture pipeline draws with no capture active: nothing lands
 *   small     3 points from counter 0: 96 bytes, counter 96
 *   order     6000 points (many workgroups): record k is vertex k
 *   resume    counter preloaded to 64, two draws (3 points, then 2 from
 *             firstVertex 3) in one begin/end: records at 64.., counter 224
 *   overflow  a 320-byte binding, 16 points: exactly 10 records, counter 320
 *   streams   stream 0 into buffer 0 and stream 1 into buffer 1, 4 points
 *   instanced 3 points, 2 instances: 6 records, instance 0 then instance 1
 *   drawauto  vkCmdDrawIndirectByteCountEXT with counter 128, counterOffset 32
 *             and stride 32: 3 vertices drawn and captured, counter 96
 * Every submission waits on a 300 ms fence; no shader loops.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t14_xfb_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { EXTENT = 16, ORDER_POINTS = 6000, CAPTURE_BYTES = 262144, COUNTER_BYTES = 64 };
#define SENTINEL UINT32_C(0xdeadbeef)
static const uint64_t fence_timeout = UINT64_C(300000000);

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }
static void value(uint32_t k, uint32_t instance, uint32_t out[4])
{
    out[0] = bits((float)k); out[1] = bits((float)(2u * k + 1u));
    out[2] = bits((float)(k & 255u)); out[3] = bits(7.0f + (float)instance);
}
static uint32_t digest_bytes(uint32_t digest, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < bytes; ++i) digest = (digest ^ p[i]) * UINT32_C(16777619);
    return digest;
}

struct host_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t *words;
    VkDeviceSize bytes;
};
static VkResult make_buffer(VkDevice device, VkDeviceSize bytes, VkBufferUsageFlags usage,
                            struct host_buffer *out)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult rc = vkCreateBuffer(device, &info, NULL, &out->buffer);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out->buffer, &req);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0,
    };
    rc = vkAllocateMemory(device, &allocation, NULL, &out->memory);
    if (rc != VK_SUCCESS) return rc;
    rc = vkBindBufferMemory(device, out->buffer, out->memory, 0);
    if (rc != VK_SUCCESS) return rc;
    out->bytes = bytes;
    return vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, (void **)&out->words);
}
static VkResult publish(VkDevice device, const struct host_buffer *b)
{
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = b->memory, .size = VK_WHOLE_SIZE};
    return vkFlushMappedMemoryRanges(device, 1, &range);
}
static VkResult observe(VkDevice device, const struct host_buffer *b)
{
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = b->memory, .size = VK_WHOLE_SIZE};
    return vkInvalidateMappedMemoryRanges(device, 1, &range);
}
static void destroy_buffer(VkDevice device, struct host_buffer *b)
{
    if (b->words) vkUnmapMemory(device, b->memory);
    if (b->buffer) vkDestroyBuffer(device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(device, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}

/* Records of `stride` bytes starting at byte `base`: record r must be vertex
 * first+r (or, with `per_instance` vertices per instance, vertex r % n of
 * instance r / n) with the layout the pipeline captures; every word from the
 * end of the last record to `sentinel_end` must still be the sentinel. */
struct tally { uint32_t mismatches, first_bad, sentinel_bad, before_bad; };
static struct tally score_instances(const uint32_t *words, uint32_t base, uint32_t records,
    uint32_t first, uint32_t stride, int swizzled, uint32_t sentinel_end, uint32_t per_instance)
{
    struct tally t = {0, UINT32_MAX, 0, 0};
    for (uint32_t w = 0; w < base / 4u; ++w) t.before_bad += words[w] != SENTINEL;
    for (uint32_t r = 0; r < records; ++r) {
        uint32_t v[4], expect[8];
        if (per_instance) value(r % per_instance, r / per_instance, v);
        else value(first + r, 0, v);
        const uint32_t *got = words + (base + r * stride) / 4u;
        if (stride == 32u) {
            memcpy(expect, v, 16);
            expect[4] = v[3]; expect[5] = v[2]; expect[6] = v[1]; expect[7] = v[0];
        } else if (swizzled) {
            expect[0] = v[1]; expect[1] = v[0]; expect[2] = v[3]; expect[3] = v[2];
        } else memcpy(expect, v, 16);
        if (memcmp(got, expect, stride)) {
            ++t.mismatches;
            if (t.first_bad == UINT32_MAX) t.first_bad = r;
        }
    }
    for (uint32_t w = (base + records * stride) / 4u; w < sentinel_end / 4u; ++w)
        t.sentinel_bad += words[w] != SENTINEL;
    return t;
}
static struct tally score(const uint32_t *words, uint32_t base, uint32_t records,
    uint32_t first, uint32_t stride, int swizzled, uint32_t sentinel_end)
{ return score_instances(words, base, records, first, stride, swizzled, sentinel_end, 0); }

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
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkShaderModule capture_geometry = VK_NULL_HANDLE, streams_geometry = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline capture = VK_NULL_HANDLE, streams = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    struct host_buffer buffer0 = {0}, buffer1 = {0}, counters = {0};
    unsigned completed = 0, passed = 0;

    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &xfb};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features2);
    VkPhysicalDeviceTransformFeedbackPropertiesEXT limits = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &limits};
    vkGetPhysicalDeviceProperties2KHR(physical, &properties2);
    ps5log_printf(PS5LOG_MARK,
        "T14_XFB_WITNESS_START feature=%u streams_feature=%u geometry=%u streams=%u "
        "buffers=%u stride=%u data=%u stream_data=%u queries=%u draw=%u",
        (unsigned)xfb.transformFeedback, (unsigned)xfb.geometryStreams,
        (unsigned)features2.features.geometryShader, limits.maxTransformFeedbackStreams,
        limits.maxTransformFeedbackBuffers, limits.maxTransformFeedbackBufferDataStride,
        limits.maxTransformFeedbackBufferDataSize, limits.maxTransformFeedbackStreamDataSize,
        (unsigned)limits.transformFeedbackQueries, (unsigned)limits.transformFeedbackDraw);
    REQUIRE(xfb.transformFeedback && xfb.geometryStreams && features2.features.geometryShader,
            "transformFeedback, geometryStreams and geometryShader reported");

    VkPhysicalDeviceTransformFeedbackFeaturesEXT enable_xfb = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
        .transformFeedback = VK_TRUE, .geometryStreams = VK_TRUE};
    VkPhysicalDeviceFeatures2 enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &enable_xfb,
        .features = {.geometryShader = VK_TRUE}};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const char *device_extension = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enable,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &device_extension,
    };
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    PFN_vkCmdBindTransformFeedbackBuffersEXT bind_xfb =
        (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(device,
            "vkCmdBindTransformFeedbackBuffersEXT");
    PFN_vkCmdBeginTransformFeedbackEXT begin_xfb =
        (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(device,
            "vkCmdBeginTransformFeedbackEXT");
    PFN_vkCmdEndTransformFeedbackEXT end_xfb =
        (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(device,
            "vkCmdEndTransformFeedbackEXT");
    PFN_vkCmdDrawIndirectByteCountEXT draw_auto =
        (PFN_vkCmdDrawIndirectByteCountEXT)vkGetDeviceProcAddr(device,
            "vkCmdDrawIndirectByteCountEXT");
    REQUIRE(bind_xfb && begin_xfb && end_xfb && draw_auto, "transform feedback entry points");

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference,
    };
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass,
    };
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {EXTENT, EXTENT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    TRY(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements image_req;
    vkGetImageMemoryRequirements(device, image, &image_req);
    VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_req.size, .memoryTypeIndex = 0,
    };
    TRY(vkAllocateMemory(device, &image_allocation, NULL, &image_memory));
    TRY(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    TRY(vkCreateImageView(device, &view_info, NULL, &view));
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view,
        .width = EXTENT, .height = EXTENT, .layers = 1,
    };
    TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    const struct { const uint32_t *code; size_t bytes; VkShaderModule *out; } modules[] = {
        {t14_xfb_vert_spirv, sizeof(t14_xfb_vert_spirv), &vertex},
        {t14_xfb_frag_spirv, sizeof(t14_xfb_frag_spirv), &fragment},
        {t14_xfb_capture_geom_spirv, sizeof(t14_xfb_capture_geom_spirv), &capture_geometry},
        {t14_xfb_streams_geom_spirv, sizeof(t14_xfb_streams_geom_spirv), &streams_geometry},
    };
    for (unsigned m = 0; m < sizeof(modules) / sizeof(modules[0]); ++m) {
        VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = modules[m].bytes, .pCode = modules[m].code};
        TRY(vkCreateShaderModule(device, &info, NULL, modules[m].out));
    }
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkViewport viewport = {0.0f, 0.0f, (float)EXTENT, (float)EXTENT, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {EXTENT, EXTENT}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    /* DXVK's stream output with no rasterized stream. */
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .rasterizerDiscardEnable = VK_TRUE, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkPipelineShaderStageCreateInfo stages[3] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = capture_geometry, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"},
    };
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 3, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = layout, .renderPass = pass, .subpass = 0,
    };
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &capture));
    stages[1].module = streams_geometry;
    TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &streams));
    ps5log_printf(PS5LOG_MARK, "T14_XFB_WITNESS_PIPELINES created=2");

    const VkBufferUsageFlags capture_usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    TRY(make_buffer(device, CAPTURE_BYTES, capture_usage, &buffer0));
    TRY(make_buffer(device, CAPTURE_BYTES, capture_usage, &buffer1));
    TRY(make_buffer(device, COUNTER_BYTES,
                    VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &counters));
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));

    enum { INACTIVE, SMALL, ORDER, RESUME, OVERFLOW, STREAMS, INSTANCED, DRAWAUTO, CASES };
    static const char *const names[CASES] = {"inactive", "small", "order", "resume",
                                             "overflow", "streams", "instanced", "drawauto"};
    for (unsigned c = 0; c < CASES; ++c) {
        /* Sentinel every capture word; counters start at 0 (64 for resume). */
        for (uint32_t w = 0; w < CAPTURE_BYTES / 4u; ++w)
            buffer0.words[w] = buffer1.words[w] = SENTINEL;
        memset(counters.words, 0, COUNTER_BYTES);
        counters.words[0] = c == RESUME ? 64u : 0u;
        /* The byte count the drawauto case draws from: (128 - 32) / 32. */
        counters.words[2] = c == DRAWAUTO ? 128u : 0u;
        TRY(publish(device, &buffer0));
        TRY(publish(device, &buffer1));
        TRY(publish(device, &counters));

        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        TRY(vkResetCommandBuffer(command, 0));
        TRY(vkBeginCommandBuffer(command, &begin));
        VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
        VkRenderPassBeginInfo pass_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
            .framebuffer = framebuffer, .renderArea = {{0, 0}, {EXTENT, EXTENT}},
            .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          c == STREAMS ? streams : capture);
        const VkBuffer bound[2] = {buffer0.buffer, buffer1.buffer};
        const VkDeviceSize offsets[2] = {0, 0};
        const VkDeviceSize sizes[2] = {c == OVERFLOW ? 320u : VK_WHOLE_SIZE, VK_WHOLE_SIZE};
        const VkBuffer counter_buffers[4] = {counters.buffer, counters.buffer,
                                             VK_NULL_HANDLE, VK_NULL_HANDLE};
        const VkDeviceSize counter_offsets[4] = {0, 4, 0, 0};
        if (c != INACTIVE) {
            bind_xfb(command, 0, c == STREAMS ? 2u : 1u, bound, offsets, sizes);
            begin_xfb(command, 0, 4, counter_buffers, counter_offsets);
        }
        const uint32_t points = c == INACTIVE || c == SMALL || c == RESUME ||
            c == INSTANCED || c == DRAWAUTO ? 3u : c == ORDER ? ORDER_POINTS :
            c == OVERFLOW ? 16u : 4u;
        if (c == DRAWAUTO) draw_auto(command, 1, 0, counters.buffer, 8, 32, 32);
        else vkCmdDraw(command, points, c == INSTANCED ? 2u : 1u, 0, 0);
        if (c == RESUME) vkCmdDraw(command, 2, 1, 3, 0);
        if (c != INACTIVE) end_xfb(command, 0, 4, counter_buffers, counter_offsets);
        vkCmdEndRenderPass(command);
        TRY(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
        TRY(vkResetFences(device, 1, &fence));
        ++completed;
        TRY(observe(device, &buffer0));
        TRY(observe(device, &buffer1));
        TRY(observe(device, &counters));

        struct tally t0, t1 = {0, UINT32_MAX, 0, 0};
        uint32_t want0 = 0, want1 = 0;
        switch (c) {
        case INACTIVE: t0 = score(buffer0.words, 0, 0, 0, 32, 0, 4096); want0 = 0; break;
        case SMALL: t0 = score(buffer0.words, 0, 3, 0, 32, 0, 4096); want0 = 96; break;
        case ORDER:
            t0 = score(buffer0.words, 0, ORDER_POINTS, 0, 32, 0, ORDER_POINTS * 32u + 4096u);
            want0 = ORDER_POINTS * 32u; break;
        case RESUME: t0 = score(buffer0.words, 64, 5, 0, 32, 0, 4096); want0 = 64 + 160; break;
        case OVERFLOW: t0 = score(buffer0.words, 0, 10, 0, 32, 0, 4096); want0 = 320; break;
        case DRAWAUTO: t0 = score(buffer0.words, 0, 3, 0, 32, 0, 4096); want0 = 96; break;
        case INSTANCED:
            t0 = score_instances(buffer0.words, 0, 6, 0, 32, 0, 4096, 3); want0 = 192; break;
        default:
            t0 = score(buffer0.words, 0, 4, 0, 16, 0, 4096);
            t1 = score(buffer1.words, 0, 4, 0, 16, 1, 4096);
            want0 = want1 = 64; break;
        }
        const uint32_t counter0 = counters.words[0], counter1 = counters.words[1];
        const int counters_ok = c == INACTIVE ? counter0 == 0 && counter1 == 0 :
            counter0 == want0 && counter1 == want1;
        const int ok = !t0.mismatches && !t0.sentinel_bad && !t0.before_bad &&
            !t1.mismatches && !t1.sentinel_bad && counters_ok;
        passed += ok ? 1u : 0u;
        ps5log_printf(PS5LOG_MARK,
            "T14_XFB_WITNESS_CASE name=%s points=%u ok=%d mismatches=%u first_bad=%d "
            "sentinel_bad=%u before_bad=%u counter0=%u want0=%u stream1_mismatches=%u "
            "stream1_sentinel_bad=%u counter1=%u want1=%u w0=%08x,%08x,%08x,%08x "
            "digest=%08x fence=complete",
            names[c], points, ok, t0.mismatches,
            t0.first_bad == UINT32_MAX ? -1 : (int)t0.first_bad,
            t0.sentinel_bad, t0.before_bad, counter0, want0, t1.mismatches, t1.sentinel_bad,
            counter1, want1, buffer0.words[0], buffer0.words[1], buffer0.words[2],
            buffer0.words[3],
            digest_bytes(UINT32_C(2166136261), buffer0.words, 8192));
    }
    ps5log_printf(PS5LOG_MARK, "T14_XFB_WITNESS_RESULT cases=%u passed=%u submissions=%u",
        (unsigned)CASES, passed, completed);

cleanup:
    if (failed)
        ps5log_printf(PS5LOG_ERR, "T14_XFB_WITNESS_FAILURE result=%d step=%s completed=%u",
            (int)result, failed, completed);
    if (device) {
        vkDeviceWaitIdle(device);
        destroy_buffer(device, &buffer0);
        destroy_buffer(device, &buffer1);
        destroy_buffer(device, &counters);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (capture) vkDestroyPipeline(device, capture, NULL);
        if (streams) vkDestroyPipeline(device, streams, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (vertex) vkDestroyShaderModule(device, vertex, NULL);
        if (fragment) vkDestroyShaderModule(device, fragment, NULL);
        if (capture_geometry) vkDestroyShaderModule(device, capture_geometry, NULL);
        if (streams_geometry) vkDestroyShaderModule(device, streams_geometry, NULL);
        if (framebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
        if (view) vkDestroyImageView(device, view, NULL);
        if (image) vkDestroyImage(device, image, NULL);
        if (image_memory) vkFreeMemory(device, image_memory, NULL);
        if (pass) vkDestroyRenderPass(device, pass, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    if (!failed)
        ps5log_printf(PS5LOG_MARK, "T14_XFB_WITNESS_RETIRED resources=clean");
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
    ps5log_close(failed ? "t14-xfb-witness-failed" : "t14-xfb-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

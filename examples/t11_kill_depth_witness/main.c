/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T11 pixel-removal witness: a public-SDK payload (no private headers or
 * symbols) that measures whether a fragment stage which removes pixels keeps
 * them out of the depth and stencil planes.
 *
 * Four cases, one combined D32_SFLOAT_S8_UINT attachment each, in one process:
 *   control    no removal (the depth-only stage that writes nothing)
 *   kill       OpKill, a Vulkan 1.0 module
 *   terminate  OpTerminateInvocation, SPIR-V 1.6 (DXVK's meta-shader discard)
 *   demote     OpDemoteToHelperInvocation, SPIR-V 1.6 (DXVK's DXBC discard)
 * Each case clears depth to 1.0 and stencil to 0xa5, then draws one full
 * target quad whose depth is the plane z = (x + 64y) / 8192, depth test LESS
 * with writes, stencil ALWAYS/REPLACE with reference 0x5a. The three removal
 * stages remove exactly the pixels where (x ^ y) & 1 is set, so a removed pixel
 * must read back 1.0 / 0xa5 and every other pixel its plane value / 0x5a.
 *
 * The control proves the instrument (every pixel written) in the same process,
 * so a removal case whose removed pixels were written is the defect itself,
 * not a broken readback. This is the regression witness for the driver's
 * kill export-memory rule: before the rule, every removed pixel was written.
 * Every submission waits on a bounded fence and no shader loops.
 *
 * Each case is three submissions, the route the T09 depth/stencil witness
 * measured: the render pass alone, then each aspect handed to TRANSFER_SRC and
 * copied in a submission of its own. A render-pass postlude carries one
 * readback, so both aspects cannot follow the pass in one command buffer.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t11_kill_depth_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { EXTENT = 64, PIXELS = EXTENT * EXTENT, CASES = 4, SUBMISSIONS_PER_CASE = 3,
       STENCIL_CLEAR = 0xa5, STENCIL_REFERENCE = 0x5a };
static const float depth_clear = 1.0f;
static const uint64_t fence_timeout = UINT64_C(300000000);

struct witness_case {
    const char *name;
    const uint32_t *code;
    size_t bytes;
    int removes;
};

static uint32_t digest_bytes(uint32_t digest, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < bytes; ++i) digest = (digest ^ p[i]) * UINT32_C(16777619);
    return digest;
}

static float expected_depth(uint32_t x, uint32_t y)
{
    return ((float)x + 0.5f + 64.0f * ((float)y + 0.5f)) / 8192.0f;
}

static int removed(uint32_t x, uint32_t y) { return ((x ^ y) & 1u) != 0; }

/* Adjacent pixels differ by 1/8192 in depth, far above the interpolation error. */
static int depth_is(float value, float expected)
{
    const float error = value - expected;
    return error > -1.0f / 65536.0f && error < 1.0f / 65536.0f;
}

struct tally {
    uint32_t depth_mismatches, stencil_mismatches, removed_written, kept_missing;
};

static struct tally score(const float *depth, const uint8_t *stencil, int removes)
{
    struct tally t = {0};
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x) {
            const uint32_t i = y * EXTENT + x;
            const int gone = removes && removed(x, y);
            const float want_depth = gone ? depth_clear : expected_depth(x, y);
            const uint8_t want_stencil = gone ? STENCIL_CLEAR : STENCIL_REFERENCE;
            const int depth_ok = depth_is(depth[i], want_depth);
            const int stencil_ok = stencil[i] == want_stencil;
            t.depth_mismatches += !depth_ok;
            t.stencil_mismatches += !stencil_ok;
            /* The two defect signatures, counted per pixel over both planes. */
            if (gone && (!depth_ok || !stencil_ok)) ++t.removed_written;
            if (!gone && (depth_is(depth[i], depth_clear) || stencil[i] == STENCIL_CLEAR))
                ++t.kept_missing;
        }
    return t;
}

struct readback {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *host;
};

static VkResult make_readback(VkDevice device, VkDeviceSize bytes, struct readback *out)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
    rc = vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->host);
    if (rc != VK_SUCCESS) return rc;
    /* A sentinel the GPU readback must overwrite. */
    memset(out->host, 0xcd, (size_t)bytes);
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = out->memory, .size = VK_WHOLE_SIZE,
    };
    return vkFlushMappedMemoryRanges(device, 1, &range);
}

static void destroy_readback(VkDevice device, struct readback *r)
{
    if (r->host) vkUnmapMemory(device, r->memory);
    if (r->buffer) vkDestroyBuffer(device, r->buffer, NULL);
    if (r->memory) vkFreeMemory(device, r->memory, NULL);
    memset(r, 0, sizeof(*r));
}

static VkResult invalidate(VkDevice device, const struct readback *r)
{
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = r->memory, .size = VK_WHOLE_SIZE,
    };
    return vkInvalidateMappedMemoryRanges(device, 1, &range);
}

/* One aspect to TRANSFER_SRC, copied into its buffer and published to the
 * host: the shipping separate-layout route the T09 witness measured. */
static void copy_aspect(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect,
    VkImageLayout attachment_layout, const struct readback *r)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = attachment_layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy region = {
        .imageSubresource = {aspect, 0, 0, 1},
        .imageExtent = {EXTENT, EXTENT, 1},
    };
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           r->buffer, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->buffer, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host, 0, NULL);
}

static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(condition, reason) do { if (!(condition)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)
    static const struct witness_case cases[CASES] = {
        {"control", t11_control_frag_spirv, sizeof(t11_control_frag_spirv), 0},
        {"kill", t11_kill_frag_spirv, sizeof(t11_kill_frag_spirv), 1},
        {"terminate", t11_terminate_frag_spirv, sizeof(t11_terminate_frag_spirv), 1},
        {"demote", t11_demote_frag_spirv, sizeof(t11_demote_frag_spirv), 1},
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkImage images[CASES] = {VK_NULL_HANDLE};
    VkDeviceMemory image_memory[CASES] = {VK_NULL_HANDLE};
    VkImageView views[CASES] = {VK_NULL_HANDLE};
    VkFramebuffer framebuffers[CASES] = {VK_NULL_HANDLE};
    VkShaderModule fragments[CASES] = {VK_NULL_HANDLE};
    VkPipeline pipelines[CASES] = {VK_NULL_HANDLE};
    VkRenderPass pass = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[CASES * SUBMISSIONS_PER_CASE] = {VK_NULL_HANDLE};
    VkFence fence = VK_NULL_HANDLE;
    struct readback depth[CASES] = {{0}}, stencil[CASES] = {{0}};
    unsigned completed = 0;

    ps5log_printf(PS5LOG_MARK, "T11_KILL_WITNESS_START extent=%u cases=%u", EXTENT,
        (unsigned)CASES);
    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures separate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES};
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &separate};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features2);
    REQUIRE(separate.separateDepthStencilLayouts, "separateDepthStencilLayouts reported");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const char *device_extensions[4] = {
        VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
        VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &separate,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 4, .ppEnabledExtensionNames = device_extensions,
    };
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pDepthStencilAttachment = &reference,
    };
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass,
    };
    TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkShaderModuleCreateInfo vertex_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t11_quad_vert_spirv), .pCode = t11_quad_vert_spirv,
    };
    TRY(vkCreateShaderModule(device, &vertex_info, NULL, &vertex));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = {0.0f, 0.0f, (float)EXTENT, (float)EXTENT, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {EXTENT, EXTENT}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    /* Both planes are written by every surviving pixel: depth LESS against
     * the 1.0 clear, stencil ALWAYS/REPLACE on both faces. */
    const VkStencilOpState replace = {
        .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE,
        .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
        .compareMask = 0xff, .writeMask = 0xff, .reference = STENCIL_REFERENCE,
    };
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS, .stencilTestEnable = VK_TRUE,
        .front = replace, .back = replace, .maxDepthBounds = 1.0f,
    };
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main"},
    };
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_state,
        .layout = layout, .renderPass = pass, .subpass = 0,
    };
    for (unsigned c = 0; c < CASES; ++c) {
        VkShaderModuleCreateInfo fragment_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = cases[c].bytes, .pCode = cases[c].code,
        };
        TRY(vkCreateShaderModule(device, &fragment_info, NULL, &fragments[c]));
        stages[1].module = fragments[c];
        TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                      &pipelines[c]));
        ps5log_printf(PS5LOG_MARK, "T11_KILL_WITNESS_PIPELINE form=%s created=1", cases[c].name);
        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
            .extent = {EXTENT, EXTENT, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        TRY(vkCreateImage(device, &image_info, NULL, &images[c]));
        VkMemoryRequirements image_req;
        vkGetImageMemoryRequirements(device, images[c], &image_req);
        VkMemoryAllocateInfo image_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = image_req.size, .memoryTypeIndex = 0,
        };
        TRY(vkAllocateMemory(device, &image_allocation, NULL, &image_memory[c]));
        TRY(vkBindImageMemory(device, images[c], image_memory[c], 0));
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[c],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
            .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                 0, 1, 0, 1},
        };
        TRY(vkCreateImageView(device, &view_info, NULL, &views[c]));
        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
            .attachmentCount = 1, .pAttachments = &views[c],
            .width = EXTENT, .height = EXTENT, .layers = 1,
        };
        TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffers[c]));
        TRY(make_readback(device, PIXELS * 4u, &depth[c]));
        TRY(make_readback(device, PIXELS, &stencil[c]));
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0,
    };
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = CASES * SUBMISSIONS_PER_CASE,
    };
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkClearValue clear = {.depthStencil = {depth_clear, STENCIL_CLEAR}};
    for (unsigned c = 0; c < CASES; ++c) {
        VkCommandBuffer *case_commands = &commands[c * SUBMISSIONS_PER_CASE];
        TRY(vkBeginCommandBuffer(case_commands[0], &begin));
        VkRenderPassBeginInfo pass_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
            .framebuffer = framebuffers[c], .renderArea = {{0, 0}, {EXTENT, EXTENT}},
            .clearValueCount = 1, .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(case_commands[0], &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(case_commands[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[c]);
        vkCmdDraw(case_commands[0], 6, 1, 0, 0);
        vkCmdEndRenderPass(case_commands[0]);
        TRY(vkEndCommandBuffer(case_commands[0]));
        TRY(vkBeginCommandBuffer(case_commands[1], &begin));
        copy_aspect(case_commands[1], images[c], VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, &depth[c]);
        TRY(vkEndCommandBuffer(case_commands[1]));
        TRY(vkBeginCommandBuffer(case_commands[2], &begin));
        copy_aspect(case_commands[2], images[c], VK_IMAGE_ASPECT_STENCIL_BIT,
                    VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL, &stencil[c]);
        TRY(vkEndCommandBuffer(case_commands[2]));
    }
    for (unsigned c = 0; c < CASES; ++c) {
        for (unsigned s = 0; s < SUBMISSIONS_PER_CASE; ++s) {
            VkSubmitInfo submit = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &commands[c * SUBMISSIONS_PER_CASE + s],
            };
            TRY(vkQueueSubmit(queue, 1, &submit, fence));
            TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
            TRY(vkResetFences(device, 1, &fence));
            ++completed;
            ps5log_printf(PS5LOG_MARK, "T11_KILL_WITNESS_STEP form=%s index=%u fence=complete",
                cases[c].name, s);
        }
        TRY(invalidate(device, &depth[c]));
        TRY(invalidate(device, &stencil[c]));
        const struct tally t = score(depth[c].host, stencil[c].host, cases[c].removes);
        const uint32_t *d = depth[c].host;
        const uint8_t *s = stencil[c].host;
        ps5log_printf(PS5LOG_MARK,
            "T11_KILL_WITNESS_CASE form=%s removes=%d depth_mismatches=%u "
            "stencil_mismatches=%u removed_written=%u kept_missing=%u "
            "depth_0_0=%08x depth_1_0=%08x stencil_0_0=%02x stencil_1_0=%02x "
            "depth_digest=%08x stencil_digest=%08x fence=complete",
            cases[c].name, cases[c].removes, t.depth_mismatches, t.stencil_mismatches,
            t.removed_written, t.kept_missing, d[0], d[1], s[0], s[1],
            digest_bytes(UINT32_C(2166136261), depth[c].host, PIXELS * 4u),
            digest_bytes(UINT32_C(2166136261), stencil[c].host, PIXELS));
    }
    ps5log_printf(PS5LOG_MARK, "T11_KILL_WITNESS_RESULT cases=%u submissions=%u fence=complete",
        (unsigned)CASES, completed);

cleanup:
    if (failed)
        ps5log_printf(PS5LOG_ERR, "T11_KILL_WITNESS_FAILURE result=%d step=%s completed=%u",
            (int)result, failed, completed);
    if (device) {
        vkDeviceWaitIdle(device);
        for (unsigned c = 0; c < CASES; ++c) {
            destroy_readback(device, &depth[c]);
            destroy_readback(device, &stencil[c]);
            if (pipelines[c]) vkDestroyPipeline(device, pipelines[c], NULL);
            if (fragments[c]) vkDestroyShaderModule(device, fragments[c], NULL);
            if (framebuffers[c]) vkDestroyFramebuffer(device, framebuffers[c], NULL);
            if (views[c]) vkDestroyImageView(device, views[c], NULL);
            if (images[c]) vkDestroyImage(device, images[c], NULL);
            if (image_memory[c]) vkFreeMemory(device, image_memory[c], NULL);
        }
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (vertex) vkDestroyShaderModule(device, vertex, NULL);
        if (pass) vkDestroyRenderPass(device, pass, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    if (!failed)
        ps5log_printf(PS5LOG_MARK, "T11_KILL_WITNESS_RETIRED resources=clean");
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
    ps5log_close(failed ? "t11-kill-depth-witness-failed" : "t11-kill-depth-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

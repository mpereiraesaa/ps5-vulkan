/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T11 helper-invocation witness: a public-SDK payload (no private headers or
 * symbols) that measures whether a demoted fragment invocation keeps running
 * as a helper, so the derivatives its quad computes afterwards stay defined.
 *
 * Every case draws one full-target quad into a 64x64 R8G8B8A8_UNORM target
 * cleared to zero. The fragment stage removes the top-left pixel of every 2x2
 * quad and then writes (dFdx(x*x), dFdy(y*y), dFdy(x*x), 1) / 255, which reads
 * the removed lane's value: a written pixel must hold
 * (2*(x & ~1) + 1, 2*(y & ~1) + 1, 0, 255) and a removed one must stay zero.
 * Cases, one pipeline each:
 *   control     no removal (the instrument: every pixel is the formula)
 *   kill        OpKill, SPIR-V 1.0
 *   demote      OpDemoteToHelperInvocation, SPIR-V 1.6, extension declared
 *   demote_dxvk the same module without OpExtension, as DXVK 2.6.2 emits it
 *   demote_ext  OpDemoteToHelperInvocationEXT, SPIR-V 1.3
 *   terminate   OpTerminateInvocation, SPIR-V 1.6 (derivatives undefined by
 *               the specification; reported, never required)
 * Each case is two submissions: the render pass, then the colour readback.
 * Every submission waits on a bounded fence and no shader loops.
 */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t11_helper_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { EXTENT = 64, PIXELS = EXTENT * EXTENT, CASES = 6, SUBMISSIONS_PER_CASE = 2 };
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

static uint32_t expected_word(uint32_t x, uint32_t y)
{
    const uint32_t r = 2u * (x & ~1u) + 1u, g = 2u * (y & ~1u) + 1u;
    return r | (g << 8) | (UINT32_C(0xff) << 24);
}

struct tally { uint32_t kept_wrong, removed_written, written; };

static struct tally score(const uint32_t *pixels, int removes)
{
    struct tally t = {0};
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x) {
            const uint32_t word = pixels[y * EXTENT + x];
            const int gone = removes && ((x | y) & 1u) == 0;
            t.written += word != 0;
            if (gone) t.removed_written += word != 0;
            else t.kept_wrong += word != expected_word(x, y);
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

static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(condition, reason) do { if (!(condition)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)
    static const struct witness_case cases[CASES] = {
        {"control", t11_helper_control_frag_spirv, sizeof(t11_helper_control_frag_spirv), 0},
        {"kill", t11_helper_kill_frag_spirv, sizeof(t11_helper_kill_frag_spirv), 1},
        {"demote", t11_helper_demote_frag_spirv, sizeof(t11_helper_demote_frag_spirv), 1},
        {"demote_dxvk", t11_helper_demote_dxvk_frag_spirv,
         sizeof(t11_helper_demote_dxvk_frag_spirv), 1},
        {"demote_ext", t11_helper_demote_ext_frag_spirv,
         sizeof(t11_helper_demote_ext_frag_spirv), 1},
        {"terminate", t11_helper_terminate_frag_spirv,
         sizeof(t11_helper_terminate_frag_spirv), 1},
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
    struct readback color[CASES] = {{0}};
    unsigned completed = 0;

    ps5log_printf(PS5LOG_MARK, "T11_HELPER_WITNESS_START extent=%u cases=%u", EXTENT,
        (unsigned)CASES);
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
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
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment,
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
        .pColorBlendState = &blend,
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
        ps5log_printf(PS5LOG_MARK, "T11_HELPER_WITNESS_PIPELINE form=%s created=1",
            cases[c].name);
        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {EXTENT, EXTENT, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        TRY(vkCreateImageView(device, &view_info, NULL, &views[c]));
        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
            .attachmentCount = 1, .pAttachments = &views[c],
            .width = EXTENT, .height = EXTENT, .layers = 1,
        };
        TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffers[c]));
        TRY(make_readback(device, PIXELS * 4u, &color[c]));
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
    VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}};
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
        /* The colour readback: the attachment to TRANSFER_SRC, the copy and
         * the host scope, in a submission of its own. */
        TRY(vkBeginCommandBuffer(case_commands[1], &begin));
        VkImageMemoryBarrier handover = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[c], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(case_commands[1], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &handover);
        VkBufferImageCopy region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {EXTENT, EXTENT, 1},
        };
        vkCmdCopyImageToBuffer(case_commands[1], images[c], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               color[c].buffer, 1, &region);
        VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = color[c].buffer, .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(case_commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        TRY(vkEndCommandBuffer(case_commands[1]));
    }
    for (unsigned c = 0; c < CASES; ++c) {
        for (unsigned step = 0; step < SUBMISSIONS_PER_CASE; ++step) {
            VkSubmitInfo submit = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &commands[c * SUBMISSIONS_PER_CASE + step],
            };
            TRY(vkQueueSubmit(queue, 1, &submit, fence));
            TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
            TRY(vkResetFences(device, 1, &fence));
            ++completed;
            ps5log_printf(PS5LOG_MARK, "T11_HELPER_WITNESS_STEP form=%s index=%u fence=complete",
                cases[c].name, step);
        }
        TRY(invalidate(device, &color[c]));
        const uint32_t *pixels = color[c].host;
        const struct tally t = score(pixels, cases[c].removes);
        /* (0,0) is removed in every removing case, (1,0), (0,1) and (1,1) are
         * its quad's helpers-dependent neighbours. */
        ps5log_printf(PS5LOG_MARK,
            "T11_HELPER_WITNESS_CASE form=%s removes=%d kept_wrong=%u removed_written=%u "
            "written=%u px_0_0=%08x px_1_0=%08x px_0_1=%08x px_1_1=%08x digest=%08x "
            "fence=complete",
            cases[c].name, cases[c].removes, t.kept_wrong, t.removed_written, t.written,
            pixels[0], pixels[1], pixels[EXTENT], pixels[EXTENT + 1],
            digest_bytes(UINT32_C(2166136261), pixels, PIXELS * 4u));
    }
    ps5log_printf(PS5LOG_MARK, "T11_HELPER_WITNESS_RESULT cases=%u submissions=%u fence=complete",
        (unsigned)CASES, completed);

cleanup:
    if (failed)
        ps5log_printf(PS5LOG_ERR, "T11_HELPER_WITNESS_FAILURE result=%d step=%s completed=%u",
            (int)result, failed, completed);
    if (device) {
        vkDeviceWaitIdle(device);
        for (unsigned c = 0; c < CASES; ++c) {
            destroy_readback(device, &color[c]);
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
        ps5log_printf(PS5LOG_MARK, "T11_HELPER_WITNESS_RETIRED resources=clean");
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
    ps5log_close(failed ? "t11-helper-witness-failed" : "t11-helper-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}

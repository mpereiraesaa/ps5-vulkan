/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public SDK consumer: no native/backend headers or private symbols.
 * Included after the consumer's CHECK/REQUIRE and owned shader declarations.
 *
 * Rasterization-state witness (DXVK262-T05): depthBiasClamp, depthClamp,
 * fillModeNonSolid and the viewport/scissor arrays of multiViewport executed
 * through the public API and judged by a CPU readback of the linear staging
 * copy of a 64 x 64 colour frame with a D32 depth attachment.
 *
 * Every case is a small scene whose outcome was derived by hand from the
 * Vulkan specification before it ran anywhere (the derivation is next to each
 * case), and every outcome differs decisively between "the state executed"
 * and "the state was dropped": a coplanar plane that only appears when its
 * bias is applied, a clamp that moves the visible boundary by sixteen
 * columns, a plane crossing both clip planes that gains thirty-two columns
 * only when depth clamping replaces clipping, a triangle whose interior pixel
 * is covered only by FILL, whose edge pixels are covered by LINE and whose
 * vertex pixels alone are covered by POINT, and a two-viewport pipeline whose
 * draw must land in bank zero and nowhere else. Nothing here reads driver
 * state: the results are the pixels the GPU wrote.
 *
 * The ViewportIndex-driven selection of a nonzero viewport needs a geometry
 * stage in core Vulkan and is NOT part of this witness; the multiViewport
 * cases here cover the array state, bank zero and the scissor banks only.
 */
enum {
    RASTER_EXTENT = 64,
    RASTER_PIXELS = RASTER_EXTENT * RASTER_EXTENT,
    RASTER_CLEAR_WORD = 0xff000000u,
    RASTER_FLOOR_WORD = 0xff604020u, /* R=20 G=40 B=60 */
    RASTER_TEST_WORD = 0xff9010e0u,  /* R=e0 G=10 B=90 */
    RASTER_PROBE_WORD = 0xff40ff40u, /* R=40 G=ff B=40 */
    RASTER_VERTEX_BYTES = 4096,
    RASTER_MAX_VERTICES = RASTER_VERTEX_BYTES / 32
};

struct raster_vertex { float position[4]; float color[4]; };

struct raster_frame {
    VkDevice device;
    VkQueue queue;
    VkCommandBuffer command;
    VkFence fence;
    VkRenderPass pass;
    VkFramebuffer framebuffer;
    VkPipelineLayout layout;
    VkShaderModule vertex_module, fragment_module;
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    struct raster_vertex *vertices;
    unsigned vertex_count;
    VkImage image, staging_image;
    VkDeviceMemory staging_memory;
    VkDeviceSize staging_bytes;
    const unsigned char *staging_rows;
    VkDeviceSize staging_pitch;
    /* Owned by raster_frame_open/close. */
    VkDeviceMemory image_memory, depth_memory;
    VkImage depth_image;
    VkImageView views[2];
    VkCommandPool pool;
};

/* Pipeline shape one case draws with. Static viewport arrays come from
 * `viewports`/`scissors` (viewport_count of each); dynamic ones from the
 * recording. */
struct raster_pipeline_desc {
    VkPolygonMode polygon_mode;
    VkCullModeFlags cull_mode;
    VkBool32 depth_test, depth_write;
    VkCompareOp depth_compare;
    VkBool32 depth_clamp;
    VkBool32 depth_bias_enable, dynamic_depth_bias;
    float depth_bias_constant, depth_bias_clamp, depth_bias_slope;
    uint32_t viewport_count;
    VkBool32 dynamic_viewport_scissor;
    const VkViewport *viewports;
    const VkRect2D *scissors;
    /* Optional geometry stage between the frame's vertex and fragment stages. */
    VkShaderModule geometry_module;
};

static void raster_color(float out[4], uint32_t word)
{
    out[0] = (float)(word & 0xffu) / 255.0f;
    out[1] = (float)((word >> 8) & 0xffu) / 255.0f;
    out[2] = (float)((word >> 16) & 0xffu) / 255.0f;
    out[3] = 1.0f;
}

/* Append one vertex: clip-space position (x, y, z, w) and the colour word. */
static void raster_vertex(struct raster_frame *f, float x, float y, float z, float w,
                          uint32_t word)
{
    REQUIRE(f->vertex_count < RASTER_MAX_VERTICES, "raster witness vertex storage");
    struct raster_vertex *v = &f->vertices[f->vertex_count++];
    v->position[0] = x; v->position[1] = y; v->position[2] = z; v->position[3] = w;
    raster_color(v->color, word);
}

/* A full-width quad (two triangles) whose depth runs linearly from `z_left`
 * at x = -1 to `z_right` at x = +1, every component scaled by `w` so the
 * post-division NDC is the same for any W. Returns its first vertex. */
static unsigned raster_quad(struct raster_frame *f, float z_left, float z_right, float w,
                            uint32_t word)
{
    const unsigned first = f->vertex_count;
    raster_vertex(f, -1.0f * w, -1.0f * w, z_left * w, w, word);
    raster_vertex(f, 1.0f * w, -1.0f * w, z_right * w, w, word);
    raster_vertex(f, -1.0f * w, 1.0f * w, z_left * w, w, word);
    raster_vertex(f, 1.0f * w, -1.0f * w, z_right * w, w, word);
    raster_vertex(f, 1.0f * w, 1.0f * w, z_right * w, w, word);
    raster_vertex(f, -1.0f * w, 1.0f * w, z_left * w, w, word);
    return first;
}

/* Clip-space coordinate of the centre of pixel column/row `p`. */
static float raster_pixel_centre(unsigned p)
{
    return ((float)p + 0.5f) / (float)RASTER_EXTENT * 2.0f - 1.0f;
}

/* A right triangle with its vertices exactly on the centres of pixels
 * (8,8), (55,8) and (8,55). Vulkan decides facing from the signed area in
 * framebuffer coordinates (x right, y down): the order (8,8) -> (55,8) ->
 * (8,55) has a positive area, i.e. it is the COUNTER-clockwise triangle that
 * VK_FRONT_FACE_COUNTER_CLOCKWISE calls front-facing, and the reverse order
 * is the clockwise one. */
static unsigned raster_triangle(struct raster_frame *f, int ccw, float z, uint32_t word)
{
    const unsigned first = f->vertex_count;
    const float a = raster_pixel_centre(8), b = raster_pixel_centre(55);
    raster_vertex(f, a, a, z, 1.0f, word);
    if (ccw) {
        raster_vertex(f, b, a, z, 1.0f, word);
        raster_vertex(f, a, b, z, 1.0f, word);
    } else {
        raster_vertex(f, a, b, z, 1.0f, word);
        raster_vertex(f, b, a, z, 1.0f, word);
    }
    return first;
}

static void raster_flush_vertices(struct raster_frame *f)
{
    VkMappedMemoryRange flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = f->vertex_memory, .offset = 0, .size = VK_WHOLE_SIZE};
    CHECK(vkFlushMappedMemoryRanges(f->device, 1, &flush));
}

static VkPipeline raster_pipeline(struct raster_frame *f, const struct raster_pipeline_desc *d)
{
    VkPipelineShaderStageCreateInfo stages[3] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = f->vertex_module, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = f->fragment_module, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = d->geometry_module, .pName = "main"}};
    const uint32_t stage_count = d->geometry_module ? 3u : 2u;
    VkVertexInputBindingDescription binding = {
        .binding = 0, .stride = sizeof(struct raster_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16}};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attributes};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = d->depth_clamp,
        .polygonMode = d->polygon_mode, .cullMode = d->cull_mode,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = d->depth_bias_enable,
        .depthBiasConstantFactor = d->depth_bias_constant,
        .depthBiasClamp = d->depth_bias_clamp,
        .depthBiasSlopeFactor = d->depth_bias_slope,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = d->viewport_count, .pViewports = d->viewports,
        .scissorCount = d->viewport_count, .pScissors = d->scissors};
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = d->depth_test, .depthWriteEnable = d->depth_write,
        .depthCompareOp = d->depth_compare};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkDynamicState dynamic_values[3];
    uint32_t dynamic_count = 0;
    if (d->dynamic_depth_bias) dynamic_values[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
    if (d->dynamic_viewport_scissor) {
        dynamic_values[dynamic_count++] = VK_DYNAMIC_STATE_VIEWPORT;
        dynamic_values[dynamic_count++] = VK_DYNAMIC_STATE_SCISSOR;
    }
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = dynamic_count, .pDynamicStates = dynamic_values};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = stage_count, .pStages = stages,
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &input_assembly,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pViewportState = &viewport_state, .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = dynamic_count ? &dynamic : NULL,
        .layout = f->layout, .renderPass = f->pass};
    VkPipeline pipeline = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(f->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                    &pipeline));
    return pipeline;
}

static void raster_submit_and_wait(struct raster_frame *f)
{
    CHECK(vkEndCommandBuffer(f->command));
    CHECK(vkResetFences(f->device, 1, &f->fence));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &f->command};
    CHECK(vkQueueSubmit(f->queue, 1, &submit, f->fence));
    CHECK(vkWaitForFences(f->device, 1, &f->fence, VK_TRUE, UINT64_C(20000000000)));
}

/* Begin one frame: colour cleared to the clear word, depth cleared to 1.0. */
static void raster_begin_frame(struct raster_frame *f)
{
    CHECK(vkResetCommandBuffer(f->command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(f->command, &begin));
    VkClearValue clears[2] = {0};
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo pass_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = f->pass, .framebuffer = f->framebuffer,
        .renderArea = {{0, 0}, {RASTER_EXTENT, RASTER_EXTENT}},
        .clearValueCount = 2, .pClearValues = clears};
    vkCmdBeginRenderPass(f->command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkDeviceSize vertex_offset = 0;
    vkCmdBindVertexBuffers(f->command, 0, 1, &f->vertex_buffer, &vertex_offset);
}

/* End the frame, then copy the colour attachment into the LINEAR staging
 * image and read its rows back (the pinned upstream readback path). */
static void raster_end_frame(struct raster_frame *f)
{
    vkCmdEndRenderPass(f->command);
    raster_submit_and_wait(f);
    CHECK(vkResetCommandBuffer(f->command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(f->command, &begin));
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier staging_in = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = f->staging_image, .subresourceRange = range};
    vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &staging_in);
    VkImageCopy copy = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {RASTER_EXTENT, RASTER_EXTENT, 1}};
    vkCmdCopyImage(f->command, f->image, VK_IMAGE_LAYOUT_GENERAL, f->staging_image,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    VkImageMemoryBarrier staging_out = staging_in;
    staging_out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    staging_out.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    staging_out.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &staging_out);
    raster_submit_and_wait(f);
    VkMappedMemoryRange invalidate = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = f->staging_memory, .offset = 0, .size = f->staging_bytes};
    CHECK(vkInvalidateMappedMemoryRanges(f->device, 1, &invalidate));
}

static uint32_t raster_pixel(const struct raster_frame *f, unsigned x, unsigned y)
{
    uint32_t word = 0;
    memcpy(&word, f->staging_rows + (VkDeviceSize)y * f->staging_pitch +
           (VkDeviceSize)x * 4u, sizeof(word));
    return word;
}

/* What one frame holds: how many pixels carry each of the four words, how
 * many carry anything else, and how many test pixels lie in each half. */
struct raster_count {
    unsigned floor, test, probe, clear, other;
    unsigned test_left, test_top;
};

static struct raster_count raster_count_frame(const struct raster_frame *f)
{
    struct raster_count c = {0};
    for (unsigned y = 0; y < RASTER_EXTENT; ++y)
        for (unsigned x = 0; x < RASTER_EXTENT; ++x) {
            const uint32_t word = raster_pixel(f, x, y);
            if (word == RASTER_FLOOR_WORD) ++c.floor;
            else if (word == RASTER_TEST_WORD) {
                ++c.test;
                if (x < RASTER_EXTENT / 2) ++c.test_left;
                if (y < RASTER_EXTENT / 2) ++c.test_top;
            } else if (word == RASTER_PROBE_WORD) ++c.probe;
            else if (word == RASTER_CLEAR_WORD) ++c.clear;
            else ++c.other;
        }
    return c;
}

static int raster_between(unsigned value, unsigned low, unsigned high)
{
    return value >= low && value <= high;
}

/* One case verdict line. `expected` names the derived expectation so the log
 * carries the oracle next to the measurement. */
static int raster_report(const char *name, const struct raster_count *c, int valid,
                         const char *expected)
{
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_RASTER case=%s floor=%u test=%u probe=%u clear=%u other=%u "
        "test_left=%u test_top=%u expected=%s valid=%d",
        name, c->floor, c->test, c->probe, c->clear, c->other, c->test_left, c->test_top,
        expected, valid);
    return valid;
}

/* Colour and D32 targets, render pass, framebuffer, linear staging image,
 * shaders, empty pipeline layout, vertex storage, command pool/buffer, fence,
 * and the GENERAL prelude on the colour target. `target_marker` names the
 * scenario's prelude line. */
static void raster_frame_open(struct raster_frame *f, VkDevice device, VkQueue queue,
                              const char *target_marker)
{
    memset(f, 0, sizeof(*f));
    f->device = device; f->queue = queue;

    /* Colour target and D32 depth target, both attachment-sized. */
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {RASTER_EXTENT, RASTER_EXTENT, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(device, &image_info, NULL, &f->image));
    VkMemoryRequirements image_requirements;
    vkGetImageMemoryRequirements(device, f->image, &image_requirements);
    VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size, .memoryTypeIndex = 0};
        CHECK(vkAllocateMemory(device, &image_allocation, NULL, &f->image_memory));
    CHECK(vkBindImageMemory(device, f->image, f->image_memory, 0));
    VkImageCreateInfo depth_info = image_info;
    depth_info.format = VK_FORMAT_D32_SFLOAT;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        CHECK(vkCreateImage(device, &depth_info, NULL, &f->depth_image));
    VkMemoryRequirements depth_requirements;
    vkGetImageMemoryRequirements(device, f->depth_image, &depth_requirements);
    VkMemoryAllocateInfo depth_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depth_requirements.size, .memoryTypeIndex = 0};
        CHECK(vkAllocateMemory(device, &depth_allocation, NULL, &f->depth_memory));
    CHECK(vkBindImageMemory(device, f->depth_image, f->depth_memory, 0));
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = f->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(device, &view_info, NULL, &f->views[0]));
    VkImageViewCreateInfo depth_view_info = view_info;
    depth_view_info.image = f->depth_image;
    depth_view_info.format = VK_FORMAT_D32_SFLOAT;
    depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    CHECK(vkCreateImageView(device, &depth_view_info, NULL, &f->views[1]));
    VkAttachmentDescription attachments[2] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference depth_reference = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color_reference,
        .pDepthStencilAttachment = &depth_reference};
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = attachments,
        .subpassCount = 1, .pSubpasses = &subpass};
    CHECK(vkCreateRenderPass(device, &pass_info, NULL, &f->pass));
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = f->pass, .attachmentCount = 2, .pAttachments = f->views,
        .width = RASTER_EXTENT, .height = RASTER_EXTENT, .layers = 1};
    CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &f->framebuffer));

    /* Linear staging image for the readback. */
    VkImageCreateInfo staging_info = image_info;
    staging_info.tiling = VK_IMAGE_TILING_LINEAR;
    staging_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CHECK(vkCreateImage(device, &staging_info, NULL, &f->staging_image));
    VkMemoryRequirements staging_requirements;
    vkGetImageMemoryRequirements(device, f->staging_image, &staging_requirements);
    VkMemoryAllocateInfo staging_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = staging_requirements.size, .memoryTypeIndex = 0};
    CHECK(vkAllocateMemory(device, &staging_allocation, NULL, &f->staging_memory));
    CHECK(vkBindImageMemory(device, f->staging_image, f->staging_memory, 0));
    f->staging_bytes = staging_requirements.size;
    unsigned char *staging_bytes = NULL;
    CHECK(vkMapMemory(device, f->staging_memory, 0, f->staging_bytes, 0, (void **)&staging_bytes));
    REQUIRE(staging_bytes != NULL, "raster witness staging map");
    f->staging_rows = staging_bytes;
    VkSubresourceLayout staging_layout = {0};
    const VkImageSubresource staging_subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    vkGetImageSubresourceLayout(device, f->staging_image, &staging_subresource, &staging_layout);
    f->staging_rows += staging_layout.offset;
    f->staging_pitch = staging_layout.rowPitch;
    REQUIRE(staging_layout.rowPitch >= RASTER_EXTENT * 4u &&
            staging_layout.offset + staging_layout.rowPitch * RASTER_EXTENT <= f->staging_bytes,
            "raster witness staging layout");

    /* Shaders and the empty pipeline layout. */
    VkShaderModuleCreateInfo vertex_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_raster_witness_vert_spirv),
        .pCode = consumer_raster_witness_vert_spirv};
    CHECK(vkCreateShaderModule(device, &vertex_info, NULL, &f->vertex_module));
    VkShaderModuleCreateInfo fragment_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_draw_parameters_frag_spirv),
        .pCode = consumer_draw_parameters_frag_spirv};
    CHECK(vkCreateShaderModule(device, &fragment_info, NULL, &f->fragment_module));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &f->layout));

    /* Host-written vertex storage. */
    VkBufferCreateInfo vertex_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = RASTER_VERTEX_BYTES,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device, &vertex_buffer_info, NULL, &f->vertex_buffer));
    VkMemoryRequirements vertex_requirements;
    vkGetBufferMemoryRequirements(device, f->vertex_buffer, &vertex_requirements);
    VkMemoryAllocateInfo vertex_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = vertex_requirements.size, .memoryTypeIndex = 0};
    CHECK(vkAllocateMemory(device, &vertex_allocation, NULL, &f->vertex_memory));
    CHECK(vkBindBufferMemory(device, f->vertex_buffer, f->vertex_memory, 0));
    CHECK(vkMapMemory(device, f->vertex_memory, 0, vertex_requirements.size, 0,
                      (void **)&f->vertices));
    memset(f->vertices, 0, (size_t)vertex_requirements.size);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
        CHECK(vkCreateCommandPool(device, &pool_info, NULL, &f->pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = f->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &command_info, &f->command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fence_info, NULL, &f->fence));
    /* The colour target enters GENERAL the way the pinned upstream draw cases
     * and the other consumer witnesses do: an UNDEFINED -> GENERAL transfer
     * transition, a clear in GENERAL and the resource-less transfer ->
     * colour-attachment barrier, in one prelude submission, so the tracked
     * layout really is GENERAL before the first frame loads it. */
    {
        CHECK(vkResetCommandBuffer(f->command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(f->command, &begin));
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier to_general = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = f->image, .subresourceRange = range};
        vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
        VkClearColorValue prelude_clear = {0};
        prelude_clear.float32[3] = 1.0f;
        vkCmdClearColorImage(f->command, f->image, VK_IMAGE_LAYOUT_GENERAL, &prelude_clear, 1, &range);
        VkMemoryBarrier to_color = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &to_color, 0, NULL, 0, NULL);
        raster_submit_and_wait(f);
        ps5log_printf(PS5LOG_MARK, "%s layout=general clear_word=%08x prelude=1", target_marker,
                      RASTER_CLEAR_WORD);
    }

}

static void raster_frame_close(struct raster_frame *f)
{
    VkDevice device = f->device;
    vkDestroyFence(device, f->fence, NULL);
    vkDestroyCommandPool(device, f->pool, NULL);
    vkUnmapMemory(device, f->vertex_memory);
    vkDestroyBuffer(device, f->vertex_buffer, NULL);
    vkFreeMemory(device, f->vertex_memory, NULL);
    vkDestroyPipelineLayout(device, f->layout, NULL);
    vkDestroyShaderModule(device, f->fragment_module, NULL);
    vkDestroyShaderModule(device, f->vertex_module, NULL);
    vkDestroyFramebuffer(device, f->framebuffer, NULL);
    vkDestroyRenderPass(device, f->pass, NULL);
    vkDestroyImageView(device, f->views[1], NULL);
    vkDestroyImageView(device, f->views[0], NULL);
    vkDestroyImage(device, f->depth_image, NULL);
    vkFreeMemory(device, f->depth_memory, NULL);
    vkDestroyImage(device, f->image, NULL);
    vkFreeMemory(device, f->image_memory, NULL);
    vkUnmapMemory(device, f->staging_memory);
    vkDestroyImage(device, f->staging_image, NULL);
    vkFreeMemory(device, f->staging_memory, NULL);
}

static void run_raster_state(VkPhysicalDevice physical, VkDevice device, VkQueue queue)
{
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_RASTER_FEATURES depthBiasClamp=%u depthClamp=%u fillModeNonSolid=%u "
        "multiViewport=%u maxViewports=%u",
        (unsigned)features.depthBiasClamp, (unsigned)features.depthClamp,
        (unsigned)features.fillModeNonSolid, (unsigned)features.multiViewport,
        properties.limits.maxViewports);
    /* The witness runs only where the four features are reported: the device
     * was created with every reported core feature enabled (main), so the
     * pipelines below negotiate exactly the state they exercise. A device
     * without them is not wrong; it simply has nothing to witness here. */
    if (!features.depthBiasClamp || !features.depthClamp || !features.fillModeNonSolid ||
        !features.multiViewport) {
        ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_SKIPPED reason=features_not_reported");
        return;
    }
    REQUIRE(properties.limits.maxViewports >= 16, "multiViewport requires maxViewports >= 16");
    /* 12 depth-bias, 8 depth-clamp, 8 polygon-mode and 3 viewport-array cases. */
    enum { CASE_COUNT = 31 };
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_RASTER_START cases=%u extent=%u clear_word=%08x floor_word=%08x "
        "test_word=%08x probe_word=%08x",
        (unsigned)CASE_COUNT, (unsigned)RASTER_EXTENT, RASTER_CLEAR_WORD, RASTER_FLOOR_WORD,
        RASTER_TEST_WORD, RASTER_PROBE_WORD);
    struct raster_frame f;
    raster_frame_open(&f, device, queue, "PS5VK_CONSUMER_RASTER_TARGET");

    /* Geometry shared by the cases, written once. */
    const unsigned floor_first = raster_quad(&f, 0.5f, 0.5f, 1.0f, RASTER_FLOOR_WORD);
    const unsigned coplanar_first = raster_quad(&f, 0.5f, 0.5f, 1.0f, RASTER_TEST_WORD);
    const unsigned tilted_first = raster_quad(&f, 0.25f, 0.75f, 1.0f, RASTER_TEST_WORD);
    const unsigned crossing_first = raster_quad(&f, -0.5f, 1.5f, 1.0f, RASTER_TEST_WORD);
    const unsigned crossing_w2_first = raster_quad(&f, -0.5f, 1.5f, 2.0f, RASTER_TEST_WORD);
    const unsigned probe_first = raster_quad(&f, 0.74f, 0.74f, 1.0f, RASTER_PROBE_WORD);
    const unsigned near_probe_first = raster_quad(&f, 0.999f, 0.999f, 1.0f, RASTER_PROBE_WORD);
    const unsigned ccw_first = raster_triangle(&f, 1, 0.5f, RASTER_TEST_WORD);
    const unsigned cw_first = raster_triangle(&f, 0, 0.5f, RASTER_TEST_WORD);
    const unsigned full_first = raster_quad(&f, 0.25f, 0.25f, 1.0f, RASTER_TEST_WORD);
    const unsigned full_probe_first = raster_quad(&f, 0.25f, 0.25f, 1.0f, RASTER_PROBE_WORD);
    raster_flush_vertices(&f);

    const VkViewport full_viewport = {0.0f, 0.0f, RASTER_EXTENT, RASTER_EXTENT, 0.0f, 1.0f};
    const VkRect2D full_scissor = {{0, 0}, {RASTER_EXTENT, RASTER_EXTENT}};
    const struct raster_pipeline_desc floor_desc = {
        .polygon_mode = VK_POLYGON_MODE_FILL, .cull_mode = VK_CULL_MODE_NONE,
        .depth_test = VK_TRUE, .depth_write = VK_TRUE, .depth_compare = VK_COMPARE_OP_LESS,
        .viewport_count = 1, .viewports = &full_viewport, .scissors = &full_scissor};
    VkPipeline floor_pipeline = raster_pipeline(&f, &floor_desc);
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_PIPELINE role=floor depth=less_write created=1");
    unsigned witnessed = 0;

    /* ---- A. Depth bias. Floor at z_f = 0.5 drawn first with LESS; the test
     * plane is coplanar (constant cases) or tilted from z_f = 0.25 at the left
     * edge to 0.75 at the right (slope cases), so m = 1/128 per pixel exactly
     * and every biased depth below stays inside [0, 1], where Vulkan defines
     * it. Column c has z_f = 0.25 + 0.5 (c + 0.5) / 64 and passes where
     * z_f + o < 0.5. Derived counts:
     *   coplanar, o = 0            -> 0 pixels (LESS fails on equality)
     *   coplanar, o < 0            -> 4096, o > 0 -> 0
     *   tilted, o = 0              -> columns 0..31 -> 2048
     *   slope -16: o = -0.125      -> z_f < 0.625: columns 0..47 -> 3072
     *   slope -16, clamp -0.0625   -> o = max(-0.125, -0.0625) = -0.0625:
     *                                 z_f < 0.5625: columns 0..39 -> 2560
     *   slope -16, clamp +0.0625   -> a positive clamp is a minimum bound and
     *                                 leaves a negative bias alone -> 3072
     *   slope +16: o = +0.125      -> z_f < 0.375: columns 0..15 -> 1024
     *   slope +16, clamp +0.0625   -> o = min(0.125, 0.0625) = 0.0625:
     *                                 z_f < 0.4375: columns 0..23 -> 1536
     * Every bound lands half a column away from a pixel centre. The constant
     * term is scaled by r (2^(e-23) for the D32 plane at 0.5); the constant
     * cases only need its sign. Sloped cases accept one column of slack for
     * the implementation's permitted approximation of m. */
    {
        struct depth_bias_case {
            const char *name; int tilted; int dynamic; VkBool32 enable;
            float constant, clamp, slope; unsigned low, high; const char *expected;
        };
        static const struct depth_bias_case cases[] = {
            {"bias_disabled_coplanar", 0, 0, VK_FALSE, 0, 0, 0, 0, 0, "0"},
            {"bias_constant_negative", 0, 0, VK_TRUE, -4.0f, 0, 0, 4096, 4096, "4096"},
            {"bias_constant_positive", 0, 0, VK_TRUE, 4.0f, 0, 0, 0, 0, "0"},
            {"bias_dynamic_constant_negative", 0, 1, VK_TRUE, -4.0f, 0, 0, 4096, 4096, "4096"},
            {"bias_disabled_tilted", 1, 0, VK_FALSE, 0, 0, 0, 2048, 2048, "2048"},
            {"bias_slope_negative", 1, 0, VK_TRUE, 0, 0, -16.0f, 3008, 3136, "3072+-64"},
            {"bias_slope_negative_clamp_negative", 1, 0, VK_TRUE, 0, -0.0625f, -16.0f, 2496, 2624, "2560+-64"},
            {"bias_slope_negative_clamp_positive", 1, 0, VK_TRUE, 0, 0.0625f, -16.0f, 3008, 3136, "3072+-64"},
            {"bias_slope_positive", 1, 0, VK_TRUE, 0, 0, 16.0f, 960, 1088, "1024+-64"},
            {"bias_slope_positive_clamp_positive", 1, 0, VK_TRUE, 0, 0.0625f, 16.0f, 1472, 1600, "1536+-64"},
            {"bias_dynamic_slope_clamp_negative", 1, 1, VK_TRUE, 0, -0.0625f, -16.0f, 2496, 2624, "2560+-64"},
        };
        for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
            const struct depth_bias_case *c = &cases[n];
            struct raster_pipeline_desc desc = floor_desc;
            desc.depth_bias_enable = c->enable;
            desc.dynamic_depth_bias = c->dynamic ? VK_TRUE : VK_FALSE;
            if (!c->dynamic) {
                desc.depth_bias_constant = c->constant; desc.depth_bias_clamp = c->clamp;
                desc.depth_bias_slope = c->slope;
            }
            VkPipeline pipeline = raster_pipeline(&f, &desc);
            raster_begin_frame(&f);
            vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, floor_pipeline);
            vkCmdDraw(f.command, 6, 1, floor_first, 0);
            vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (c->dynamic) vkCmdSetDepthBias(f.command, c->constant, c->clamp, c->slope);
            vkCmdDraw(f.command, 6, 1, c->tilted ? tilted_first : coplanar_first, 0);
            raster_end_frame(&f);
            const struct raster_count count = raster_count_frame(&f);
            const int valid = raster_between(count.test, c->low, c->high) &&
                count.floor + count.test == RASTER_PIXELS && !count.other && !count.clear;
            witnessed += raster_report(c->name, &count, valid, c->expected);
            vkDestroyPipeline(device, pipeline, NULL);
        }
        /* Two draws under one dynamic pipeline: the second setter must not
         * reach the first draw. Frame: tilted plane with (0, 0, -16) -> 3072
         * test columns 0..47 and the floor left in 48..63 (1024); then the flat
         * probe plane at z_f = 0.74 with (0, -0.0625, -16): flat means m = 0,
         * so its bias is zero and 0.74 fails LESS against every stored depth
         * (at most 0.625, or the floor's 0.5) -> probe 0. Had the second setter
         * leaked into the first draw, its clamp would have cut the test plane
         * to 2560 columns. */
        {
            struct raster_pipeline_desc desc = floor_desc;
            desc.depth_bias_enable = VK_TRUE; desc.dynamic_depth_bias = VK_TRUE;
            VkPipeline pipeline = raster_pipeline(&f, &desc);
            raster_begin_frame(&f);
            vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, floor_pipeline);
            vkCmdDraw(f.command, 6, 1, floor_first, 0);
            vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdSetDepthBias(f.command, 0.0f, 0.0f, -16.0f);
            vkCmdDraw(f.command, 6, 1, tilted_first, 0);
            vkCmdSetDepthBias(f.command, 0.0f, -0.0625f, -16.0f);
            vkCmdDraw(f.command, 6, 1, probe_first, 0);
            raster_end_frame(&f);
            const struct raster_count count = raster_count_frame(&f);
            const int valid = raster_between(count.test, 3008, 3136) && !count.probe &&
                !count.other && count.test + count.floor == RASTER_PIXELS;
            witnessed += raster_report("bias_dynamic_two_draws", &count, valid,
                                       "test=3072+-64,probe=0");
            vkDestroyPipeline(device, pipeline, NULL);
        }
    }

    /* ---- B. Depth clamp. The crossing plane runs from z_d = -0.5 at the
     * left edge to +1.5 at the right, so with clipping only columns whose
     * centre has 0 <= z_d <= 1 survive: columns 16..47 -> 2048. With
     * depthClampEnable the plane is not clipped and z_f is clamped to the
     * viewport's [min(n,f), max(n,f)]:
     *   viewport [0,1], clear 1.0, LESS: columns 48..63 clamp to exactly 1.0
     *                                     and fail LESS -> 3072 test pixels
     *   viewport [0.25,0.75]: z_f = 0.25 + 0.5 z_d in [-0, 1] clamps to the
     *                          range, LESS against 1.0 passes -> 4096; a probe
     *                          plane at z_f = 0.74 drawn with GREATER and no
     *                          write then passes where the stored depth is
     *                          below 0.74: z_d < 0.98 -> columns 0..46 -> 3008
     *                          (control, clipped: columns 16..46 -> 1984)
     *   viewport [0.75,0.25]: the same interval mirrored, z_f = 0.75 - 0.5 z_d;
     *                          the probe passes for z_d > 0.02 -> columns
     *                          17..63 -> 3008, of which the left half holds
     *                          columns 17..31 -> 960 (unmirrored: 2048)
     *   W = 2 for every clip coordinate: identical NDC, same counts. */
    {
        const VkViewport narrow = {0.0f, 0.0f, RASTER_EXTENT, RASTER_EXTENT, 0.25f, 0.75f};
        const VkViewport reversed = {0.0f, 0.0f, RASTER_EXTENT, RASTER_EXTENT, 0.75f, 0.25f};
        struct depth_clamp_case {
            const char *name; VkBool32 clamp; const VkViewport *viewport; unsigned first;
            int probe; unsigned test_low, test_high, probe_expected, probe_left_low, probe_left_high;
            const char *expected;
        };
        const struct depth_clamp_case cases[] = {
            {"clamp_disabled_control", VK_FALSE, &full_viewport, 0, 0, 2048, 2048, 0, 0, 0, "test=2048"},
            {"clamp_enabled", VK_TRUE, &full_viewport, 0, 0, 3072, 3072, 0, 0, 0, "test=3072"},
            {"clamp_enabled_w2", VK_TRUE, &full_viewport, 1, 0, 3072, 3072, 0, 0, 0, "test=3072"},
            {"clamp_disabled_narrow_probe", VK_FALSE, &narrow, 0, 1, 2048, 2048, 1984, 0, 0, "test=2048,probe=1984"},
            {"clamp_enabled_narrow_probe", VK_TRUE, &narrow, 0, 1, 4096, 4096, 3008, 0, 0, "test=4096,probe=3008"},
            {"clamp_enabled_reversed_probe", VK_TRUE, &reversed, 0, 1, 4096, 4096, 3008, 960, 960, "test=4096,probe=3008,probe_left=960"},
        };
        /* The probe plane is drawn with GREATER and no depth write through a
         * pipeline whose viewport is the full [0,1] range, so its z_f is
         * exactly 0.74 whatever range the clamped plane used. */
        struct raster_pipeline_desc probe_desc = floor_desc;
        probe_desc.depth_write = VK_FALSE; probe_desc.depth_compare = VK_COMPARE_OP_GREATER;
        VkPipeline probe_pipeline = raster_pipeline(&f, &probe_desc);
        for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
            const struct depth_clamp_case *c = &cases[n];
            struct raster_pipeline_desc desc = floor_desc;
            desc.depth_clamp = c->clamp; desc.viewports = c->viewport;
            VkPipeline pipeline = raster_pipeline(&f, &desc);
            raster_begin_frame(&f);
            vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdDraw(f.command, 6, 1, c->first ? crossing_w2_first : crossing_first, 0);
            if (c->probe) {
                vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, probe_pipeline);
                vkCmdDraw(f.command, 6, 1, probe_first, 0);
            }
            raster_end_frame(&f);
            struct raster_count count = raster_count_frame(&f);
            /* The probe overwrites test pixels it passes on; judge the test
             * plane by test + probe where a probe was drawn. */
            unsigned probe_left = 0;
            if (c->probe)
                for (unsigned y = 0; y < RASTER_EXTENT; ++y)
                    for (unsigned x = 0; x < RASTER_EXTENT / 2; ++x)
                        probe_left += raster_pixel(&f, x, y) == RASTER_PROBE_WORD;
            const unsigned plane = count.test + (c->probe ? count.probe : 0);
            int valid = raster_between(plane, c->test_low, c->test_high) && !count.other &&
                !count.floor && plane + count.clear == RASTER_PIXELS;
            if (c->probe) {
                valid = valid && count.probe == c->probe_expected &&
                    raster_between(probe_left, c->probe_left_low, c->probe_left_high);
            } else {
                valid = valid && !count.probe;
            }
            witnessed += raster_report(c->name, &count, valid, c->expected);
            vkDestroyPipeline(device, pipeline, NULL);
        }
        vkDestroyPipeline(device, probe_pipeline, NULL);
        /* Clamped depth values are real depth-buffer contents: after the
         * clamped full-range plane, a near probe at z_f = 0.999 with LESS
         * passes exactly where the stored depth is 1.0 - the clamped columns
         * 48..63 (the clear also holds 1.0 but nothing was cleared away in
         * those columns) -> 1024 probe pixels; the unclamped control leaves
         * the clear's 1.0 in columns 0..15 and 48..63 -> 2048. */
        {
            struct raster_pipeline_desc near_desc = floor_desc;
            near_desc.depth_write = VK_FALSE;
            VkPipeline near_pipeline = raster_pipeline(&f, &near_desc);
            for (unsigned clamp = 0; clamp < 2; ++clamp) {
                struct raster_pipeline_desc desc = floor_desc;
                desc.depth_clamp = clamp ? VK_TRUE : VK_FALSE;
                VkPipeline pipeline = raster_pipeline(&f, &desc);
                raster_begin_frame(&f);
                vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdDraw(f.command, 6, 1, crossing_first, 0);
                vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, near_pipeline);
                vkCmdDraw(f.command, 6, 1, near_probe_first, 0);
                raster_end_frame(&f);
                const struct raster_count count = raster_count_frame(&f);
                const int valid = count.probe == (clamp ? 1024u : 2048u) &&
                    count.test == (clamp ? 3072u : 2048u) && !count.other && !count.floor;
                witnessed += raster_report(clamp ? "clamp_enabled_near_probe" :
                    "clamp_disabled_near_probe", &count, valid,
                    clamp ? "test=3072,probe=1024" : "test=2048,probe=2048");
                vkDestroyPipeline(device, pipeline, NULL);
            }
            vkDestroyPipeline(device, near_pipeline, NULL);
        }
    }

    /* ---- C. Polygon modes. The counter-clockwise right triangle has its
     * vertices on the centres of pixels (8,8), (55,8), (8,55). FILL covers the
     * pixels with (x-8)+(y-8) < 47 plus whatever the edge rule admits on the
     * hypotenuse: 1128 with the diamond/top-left rule, so 1080..1176 is
     * accepted, and pixel (20,20) is inside. LINE covers only the three edges
     * (~47 pixels each, about 141) and leaves (20,20) clear; POINT covers the
     * three vertex pixels and nothing else. Culling is decided on the polygon
     * before the mode applies: with cullMode FRONT the CCW triangle disappears
     * in every mode, and the clockwise copy with cullMode BACK does too. */
    {
        struct polygon_case {
            const char *name; VkPolygonMode mode; VkCullModeFlags cull; int ccw;
            unsigned low, high; int centre, vertices; const char *expected;
        };
        static const struct polygon_case cases[] = {
            {"polygon_fill", VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, 1, 1080, 1176, 1, 1, "1128+-48,centre,vertices"},
            {"polygon_line", VK_POLYGON_MODE_LINE, VK_CULL_MODE_NONE, 1, 120, 170, 0, 1, "141+-25,no_centre,vertices"},
            {"polygon_point", VK_POLYGON_MODE_POINT, VK_CULL_MODE_NONE, 1, 3, 3, 0, 1, "3,vertices_only"},
            {"polygon_line_cull_front", VK_POLYGON_MODE_LINE, VK_CULL_MODE_FRONT_BIT, 1, 0, 0, 0, 0, "0"},
            {"polygon_point_cull_back_cw", VK_POLYGON_MODE_POINT, VK_CULL_MODE_BACK_BIT, 0, 0, 0, 0, 0, "0"},
            {"polygon_line_cull_back_ccw", VK_POLYGON_MODE_LINE, VK_CULL_MODE_BACK_BIT, 1, 120, 170, 0, 1, "141+-25,no_centre,vertices"},
        };
        for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
            const struct polygon_case *c = &cases[n];
            struct raster_pipeline_desc desc = floor_desc;
            desc.polygon_mode = c->mode; desc.cull_mode = c->cull;
            desc.depth_test = VK_FALSE; desc.depth_write = VK_FALSE;
            VkPipeline pipeline = raster_pipeline(&f, &desc);
            raster_begin_frame(&f);
            vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdDraw(f.command, 3, 1, c->ccw ? ccw_first : cw_first, 0);
            raster_end_frame(&f);
            const struct raster_count count = raster_count_frame(&f);
            const int centre = raster_pixel(&f, 20, 20) == RASTER_TEST_WORD;
            const int vertices = raster_pixel(&f, 8, 8) == RASTER_TEST_WORD &&
                raster_pixel(&f, 55, 8) == RASTER_TEST_WORD &&
                raster_pixel(&f, 8, 55) == RASTER_TEST_WORD;
            const int valid = raster_between(count.test, c->low, c->high) && !count.other &&
                centre == c->centre && vertices == c->vertices &&
                count.test + count.clear == RASTER_PIXELS;
            witnessed += raster_report(c->name, &count, valid, c->expected);
            vkDestroyPipeline(device, pipeline, NULL);
        }
        /* Bias applies to the polygon whatever its mode: the coplanar LINE
         * triangle over the floor is rejected by LESS without bias and drawn
         * with a negative constant bias. */
        for (unsigned biased = 0; biased < 2; ++biased) {
            struct raster_pipeline_desc desc = floor_desc;
            desc.polygon_mode = VK_POLYGON_MODE_LINE;
            desc.depth_bias_enable = biased ? VK_TRUE : VK_FALSE;
            desc.depth_bias_constant = -4.0f;
            VkPipeline pipeline = raster_pipeline(&f, &desc);
            raster_begin_frame(&f);
            vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, floor_pipeline);
            vkCmdDraw(f.command, 6, 1, floor_first, 0);
            vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdDraw(f.command, 3, 1, ccw_first, 0);
            raster_end_frame(&f);
            const struct raster_count count = raster_count_frame(&f);
            const int valid = (biased ? raster_between(count.test, 120, 170) : count.test == 0) &&
                count.floor + count.test == RASTER_PIXELS && !count.other;
            witnessed += raster_report(biased ? "polygon_line_coplanar_biased" :
                "polygon_line_coplanar_unbiased", &count, valid, biased ? "141+-25" : "0");
            vkDestroyPipeline(device, pipeline, NULL);
        }
    }

    /* ---- D. Viewport arrays, bank zero. A two-viewport pipeline whose
     * viewport 0 is the left half and viewport 1 the right half draws a
     * full-screen plane without any ViewportIndex output: Vulkan routes it to
     * viewport 0, so exactly the left 32 columns are covered (2048, all in the
     * left half). A broadcast to every bank, a bank-1 selection or a wrong
     * scissor bank would cover the right half. The dynamic case sets viewport
     * 1 first and viewport 0 second (partial updates), draws, then moves
     * viewport 0 to the top half and draws the probe colour: the probe fills
     * the top half (2048) and the test colour survives in the bottom-left
     * quadrant (1024). The scissor case keeps viewport 0 full-screen and
     * shrinks scissor 0 to the centre 16 x 16 (256) while scissor 1 stays
     * full, which a wrong scissor bank would ignore. */
    {
        const VkViewport halves[2] = {
            {0.0f, 0.0f, RASTER_EXTENT / 2, RASTER_EXTENT, 0.0f, 1.0f},
            {RASTER_EXTENT / 2, 0.0f, RASTER_EXTENT / 2, RASTER_EXTENT, 0.0f, 1.0f}};
        const VkRect2D half_scissors[2] = {
            {{0, 0}, {RASTER_EXTENT / 2, RASTER_EXTENT}},
            {{RASTER_EXTENT / 2, 0}, {RASTER_EXTENT / 2, RASTER_EXTENT}}};
        struct raster_pipeline_desc desc = floor_desc;
        desc.depth_test = VK_FALSE; desc.depth_write = VK_FALSE;
        desc.viewport_count = 2; desc.viewports = halves; desc.scissors = half_scissors;
        VkPipeline pipeline = raster_pipeline(&f, &desc);
        ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_PIPELINE role=two_viewports created=1");
        raster_begin_frame(&f);
        vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(f.command, 6, 1, full_first, 0);
        raster_end_frame(&f);
        struct raster_count count = raster_count_frame(&f);
        int valid = count.test == 2048 && count.test_left == 2048 && !count.other &&
            count.clear == 2048;
        witnessed += raster_report("viewport_static_bank0_of_two", &count, valid,
                                   "test=2048,left=2048");
        vkDestroyPipeline(device, pipeline, NULL);

        desc.dynamic_viewport_scissor = VK_TRUE; desc.viewports = NULL; desc.scissors = NULL;
        pipeline = raster_pipeline(&f, &desc);
        raster_begin_frame(&f);
        vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdSetViewport(f.command, 1, 1, &halves[1]);
        vkCmdSetScissor(f.command, 1, 1, &half_scissors[1]);
        vkCmdSetViewport(f.command, 0, 1, &halves[0]);
        vkCmdSetScissor(f.command, 0, 1, &half_scissors[0]);
        vkCmdDraw(f.command, 6, 1, full_first, 0);
        const VkViewport top = {0.0f, 0.0f, RASTER_EXTENT, RASTER_EXTENT / 2, 0.0f, 1.0f};
        const VkRect2D top_scissor = {{0, 0}, {RASTER_EXTENT, RASTER_EXTENT / 2}};
        vkCmdSetViewport(f.command, 0, 1, &top);
        vkCmdSetScissor(f.command, 0, 1, &top_scissor);
        vkCmdDraw(f.command, 6, 1, full_probe_first, 0);
        raster_end_frame(&f);
        count = raster_count_frame(&f);
        valid = count.probe == 2048 && count.test == 1024 && count.test_left == 1024 &&
            count.test_top == 0 && !count.other && count.clear == 1024;
        witnessed += raster_report("viewport_dynamic_partial_updates", &count, valid,
                                   "probe=2048,test=1024,left,bottom");
        vkDestroyPipeline(device, pipeline, NULL);

        const VkViewport full_pair[2] = {full_viewport, full_viewport};
        const VkRect2D centre_scissors[2] = {
            {{RASTER_EXTENT / 2 - 8, RASTER_EXTENT / 2 - 8}, {16, 16}}, full_scissor};
        desc.dynamic_viewport_scissor = VK_FALSE;
        desc.viewports = full_pair; desc.scissors = centre_scissors;
        pipeline = raster_pipeline(&f, &desc);
        raster_begin_frame(&f);
        vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(f.command, 6, 1, full_first, 0);
        raster_end_frame(&f);
        count = raster_count_frame(&f);
        valid = count.test == 256 && !count.other && count.clear == RASTER_PIXELS - 256 &&
            raster_pixel(&f, RASTER_EXTENT / 2 - 8, RASTER_EXTENT / 2 - 8) == RASTER_TEST_WORD &&
            raster_pixel(&f, RASTER_EXTENT / 2 + 7, RASTER_EXTENT / 2 + 7) == RASTER_TEST_WORD &&
            raster_pixel(&f, RASTER_EXTENT / 2 - 9, RASTER_EXTENT / 2) == RASTER_CLEAR_WORD &&
            raster_pixel(&f, RASTER_EXTENT / 2 + 8, RASTER_EXTENT / 2) == RASTER_CLEAR_WORD;
        witnessed += raster_report("viewport_static_scissor_bank0_of_two", &count, valid,
                                   "test=256,centre_16x16");
        vkDestroyPipeline(device, pipeline, NULL);
    }

    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_RESULT cases=%u witnessed=%u valid=%d",
                  (unsigned)CASE_COUNT, witnessed, witnessed == CASE_COUNT);
    vkDestroyPipeline(device, floor_pipeline, NULL);
    raster_frame_close(&f);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_RETIRED cases=%u witnessed=%u",
                  (unsigned)CASE_COUNT, witnessed);
}

/* multiViewport end to end: sixteen viewports laid out as a 4 x 4 grid of
 * 16 x 16 tiles, one draw of sixteen triangles, and a geometry stage that turns
 * primitive i into a full-viewport quad routed to viewport i. Tile i must hold
 * exactly the colour of input triangle i and every tile must differ: a draw
 * that ignored the index paints only tile 0 with the last colour, a broadcast
 * paints every tile with the last colour, a wrong bank permutes the tiles, and
 * a wrong scissor bank leaks colour outside a tile. Core Vulkan lets only a
 * geometry stage write ViewportIndex, so the scenario runs only where the
 * device reports geometryShader and multiViewport; it logs SKIPPED otherwise. */
static uint32_t raster_tile_word(unsigned tile)
{
    /* Exact UNORM8 channels: R = 16 t + 8, G = 247 - 16 t, B = 0x80. */
    return 0xff800000u | ((247u - 16u * tile) << 8) | (16u * tile + 8u);
}

static void run_raster_viewport_index(VkPhysicalDevice physical, VkDevice device, VkQueue queue)
{
    enum { TILES = 16, TILE = RASTER_EXTENT / 4 };
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_RASTER_GS_FEATURES geometryShader=%u multiViewport=%u maxViewports=%u",
        (unsigned)features.geometryShader, (unsigned)features.multiViewport,
        properties.limits.maxViewports);
    if (!features.geometryShader || !features.multiViewport ||
        properties.limits.maxViewports < TILES) {
        ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_GS_SKIPPED reason=features_not_reported");
        return;
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_RASTER_GS_START cases=1 extent=%u tiles=%u clear_word=%08x",
        (unsigned)RASTER_EXTENT, (unsigned)TILES, RASTER_CLEAR_WORD);
    struct raster_frame f;
    raster_frame_open(&f, device, queue, "PS5VK_CONSUMER_RASTER_GS_TARGET");
    VkShaderModuleCreateInfo geometry_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_raster_viewport_index_geom_spirv),
        .pCode = consumer_raster_viewport_index_geom_spirv};
    VkShaderModule geometry_module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &geometry_info, NULL, &geometry_module));
    /* Sixteen small triangles, one per primitive, each carrying its tile's
     * colour; the geometry stage ignores their positions. */
    for (unsigned t = 0; t < TILES; ++t) {
        const uint32_t word = raster_tile_word(t);
        raster_vertex(&f, -0.5f, -0.5f, 0.5f, 1.0f, word);
        raster_vertex(&f, 0.5f, -0.5f, 0.5f, 1.0f, word);
        raster_vertex(&f, -0.5f, 0.5f, 0.5f, 1.0f, word);
    }
    raster_flush_vertices(&f);
    VkViewport viewports[TILES]; VkRect2D scissors[TILES];
    for (unsigned t = 0; t < TILES; ++t) {
        viewports[t] = (VkViewport){(float)((t % 4) * TILE), (float)((t / 4) * TILE), TILE, TILE, 0.0f, 1.0f};
        scissors[t] = (VkRect2D){{(int32_t)((t % 4) * TILE), (int32_t)((t / 4) * TILE)}, {TILE, TILE}};
    }
    struct raster_pipeline_desc desc = {
        .polygon_mode = VK_POLYGON_MODE_FILL, .cull_mode = VK_CULL_MODE_NONE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .viewport_count = TILES, .viewports = viewports, .scissors = scissors,
        .geometry_module = geometry_module};
    VkPipeline pipeline = raster_pipeline(&f, &desc);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_GS_PIPELINE stages=3 viewports=%u created=1",
                  (unsigned)TILES);
    raster_begin_frame(&f);
    vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(f.command, 3 * TILES, 1, 0, 0);
    raster_end_frame(&f);
    unsigned matched = 0, foreign = 0, clear = 0, other = 0;
    for (unsigned y = 0; y < RASTER_EXTENT; ++y)
        for (unsigned x = 0; x < RASTER_EXTENT; ++x) {
            const unsigned tile = (y / TILE) * 4 + x / TILE;
            const uint32_t word = raster_pixel(&f, x, y);
            if (word == raster_tile_word(tile)) ++matched;
            else if (word == RASTER_CLEAR_WORD) ++clear;
            else {
                int known = 0;
                for (unsigned t = 0; t < TILES; ++t) known |= word == raster_tile_word(t);
                if (known) ++foreign; else ++other;
            }
        }
    const int valid = matched == RASTER_PIXELS && !foreign && !clear && !other;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_RASTER_GS case=viewport_index_routing tiles=%u matched=%u foreign=%u "
        "clear=%u other=%u valid=%d",
        (unsigned)TILES, matched, foreign, clear, other, valid);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_GS_RESULT cases=1 witnessed=%u valid=%d",
                  (unsigned)valid, valid);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, geometry_module, NULL);
    raster_frame_close(&f);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_RASTER_GS_RETIRED cases=1 witnessed=%u",
                  (unsigned)valid);
}

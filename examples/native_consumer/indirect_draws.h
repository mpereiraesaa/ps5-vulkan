/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public SDK consumer: no native/backend headers or private symbols.
 * Included after the consumer's CHECK/REQUIRE and owned shader declarations.
 *
 * Indirect and indexed draw witness (DXVK262-T03): drawIndirectFirstInstance,
 * multiDrawIndirect and fullDrawIndexUint32 executed through the public API
 * and judged by a CPU readback of the linear staging copy of the frame.
 *
 * The 256 x 256 colour target is a grid of cells; every draw or instance
 * places one triangle around the centre of the cell it selects and encodes
 * what the GPU delivered into that triangle's colour (see
 * experiments/graphics/runtime_indirect_witness.vert). Each case therefore
 * pins, per cell, exactly one expected word and requires every other cell to
 * hold the clear word with no stray coverage. Nothing here reads driver
 * state: the results are the pixels the GPU wrote.
 */
enum {
    INDIRECT_EXTENT = 256,
    INDIRECT_CLEAR_WORD = 0xff404040u,
    /* 65535 packed VkDrawIndirectCommand structures fit this argument buffer;
     * the other cases use its first kilobyte at non-trivial offsets. */
    INDIRECT_ARGUMENT_BYTES = 1048576,
    INDIRECT_MAX_COMMANDS = 65535,
    INDIRECT_INDEX_BYTES = 512,
    INDIRECT_VERTEX_BYTES = 512,
    INDIRECT_MAX_EXPECTED = 65536
};

struct indirect_push {
    uint32_t cells, select, encode, marker;
};

/* One frame: the recording callback issues the draws inside the render pass. */
struct indirect_frame {
    VkDevice device;
    VkQueue queue;
    VkCommandBuffer command;
    VkFence fence;
    VkRenderPass pass;
    VkFramebuffer framebuffer;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkBuffer vertex_buffer, index_buffer, argument_buffer;
    VkDeviceMemory index_memory, argument_memory;
    VkImage image, staging_image;
    VkDeviceMemory staging_memory;
    VkDeviceSize staging_bytes;
    const unsigned char *staging_rows;
    VkDeviceSize staging_pitch;
    uint32_t *arguments;
    unsigned char *indices;
};

static uint32_t indirect_word(uint32_t r, uint32_t g, uint32_t b)
{
    return 0xff000000u | ((b & 0xffu) << 16) | ((g & 0xffu) << 8) | (r & 0xffu);
}

static void indirect_flush(VkDevice device, VkDeviceMemory memory)
{
    VkMappedMemoryRange flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .offset = 0, .size = VK_WHOLE_SIZE};
    CHECK(vkFlushMappedMemoryRanges(device, 1, &flush));
}

static void indirect_submit_and_wait(struct indirect_frame *f)
{
    CHECK(vkEndCommandBuffer(f->command));
    CHECK(vkResetFences(f->device, 1, &f->fence));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &f->command};
    CHECK(vkQueueSubmit(f->queue, 1, &submit, f->fence));
    CHECK(vkWaitForFences(f->device, 1, &f->fence, VK_TRUE, UINT64_C(20000000000)));
}

/* The pinned upstream readback path: the attachment copied into a LINEAR
 * staging image whose rows are read through vkGetImageSubresourceLayout. */
static void indirect_readback(struct indirect_frame *f)
{
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
        .extent = {INDIRECT_EXTENT, INDIRECT_EXTENT, 1}};
    vkCmdCopyImage(f->command, f->image, VK_IMAGE_LAYOUT_GENERAL, f->staging_image,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    VkImageMemoryBarrier staging_out = staging_in;
    staging_out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    staging_out.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    staging_out.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &staging_out);
    indirect_submit_and_wait(f);
    VkMappedMemoryRange invalidate = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = f->staging_memory, .offset = 0, .size = f->staging_bytes};
    CHECK(vkInvalidateMappedMemoryRanges(f->device, 1, &invalidate));
}

static uint32_t indirect_pixel(const struct indirect_frame *f, unsigned x, unsigned y)
{
    uint32_t word = 0;
    memcpy(&word, f->staging_rows + (VkDeviceSize)y * f->staging_pitch +
           (VkDeviceSize)x * 4u, sizeof(word));
    return word;
}

/* Judge one frame against its expected cells: every expected cell's centre
 * carries its word and holds coverage, every other cell's centre is the clear
 * word and carries no coverage at all. Cells are `cells` per row, in the
 * image's own row order (the shader maps cell k to column k % cells and row
 * k / cells from the top). */
static int indirect_judge(const struct indirect_frame *f, const char *name,
                          uint32_t cells, const uint32_t *expected_words,
                          uint32_t expected_count)
{
    const unsigned cell_pixels = INDIRECT_EXTENT / cells;
    const uint32_t total_cells = cells * cells;
    static unsigned char coverage[INDIRECT_MAX_EXPECTED];
    memset(coverage, 0, sizeof(coverage));
    unsigned covered = 0;
    for (unsigned y = 0; y < INDIRECT_EXTENT; ++y)
        for (unsigned x = 0; x < INDIRECT_EXTENT; ++x) {
            if (indirect_pixel(f, x, y) == INDIRECT_CLEAR_WORD) continue;
            ++covered;
            const uint32_t cell = (y / cell_pixels) * cells + x / cell_pixels;
            if (coverage[cell] < 255) ++coverage[cell];
        }
    unsigned matched = 0, wrong = 0, stray = 0;
    for (uint32_t cell = 0; cell < total_cells; ++cell) {
        const unsigned cx = (cell % cells) * cell_pixels + cell_pixels / 2;
        const unsigned cy = (cell / cells) * cell_pixels + cell_pixels / 2;
        const uint32_t centre = indirect_pixel(f, cx, cy);
        /* An expected word equal to the clear word would be unprovable; the
         * cases never pin one, and this makes that a failure rather than an
         * accident. */
        if (cell < expected_count && expected_words[cell] != INDIRECT_CLEAR_WORD) {
            if (centre == expected_words[cell] && coverage[cell]) ++matched;
            else ++wrong;
        } else if (centre != INDIRECT_CLEAR_WORD || coverage[cell]) {
            ++stray;
        }
    }
    unsigned pinned = 0;
    for (uint32_t cell = 0; cell < expected_count; ++cell)
        pinned += expected_words[cell] != INDIRECT_CLEAR_WORD;
    const int valid = matched == pinned && !wrong && !stray && covered >= pinned;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_INDIRECT case=%s cells=%u expected=%u matched=%u wrong=%u "
        "stray=%u covered=%u valid=%d",
        name, cells, pinned, matched, wrong, stray, covered, valid);
    return valid;
}

static void indirect_begin_frame(struct indirect_frame *f, const struct indirect_push *push,
                                 VkIndexType index_type, int indexed)
{
    CHECK(vkResetCommandBuffer(f->command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(f->command, &begin));
    VkClearValue clear = {0};
    clear.color.float32[0] = clear.color.float32[1] = clear.color.float32[2] = 0x40 / 255.0f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = f->pass, .framebuffer = f->framebuffer,
        .renderArea = {{0, 0}, {INDIRECT_EXTENT, INDIRECT_EXTENT}},
        .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(f->command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(f->command, VK_PIPELINE_BIND_POINT_GRAPHICS, f->pipeline);
    vkCmdPushConstants(f->command, f->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(*push), push);
    const VkDeviceSize vertex_offset = 0;
    vkCmdBindVertexBuffers(f->command, 0, 1, &f->vertex_buffer, &vertex_offset);
    if (indexed) vkCmdBindIndexBuffer(f->command, f->index_buffer, 0, index_type);
}

static void indirect_end_frame(struct indirect_frame *f)
{
    vkCmdEndRenderPass(f->command);
    indirect_submit_and_wait(f);
    indirect_readback(f);
}

static void indirect_write_arguments(struct indirect_frame *f, VkDeviceSize offset,
                                     const void *data, size_t bytes)
{
    memcpy((unsigned char *)f->arguments + offset, data, bytes);
}

static void run_indirect_draws(VkPhysicalDevice physical, VkDevice device, VkQueue queue)
{
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_INDIRECT_FEATURES multiDrawIndirect=%u drawIndirectFirstInstance=%u "
        "fullDrawIndexUint32=%u maxDrawIndirectCount=%u maxDrawIndexedIndexValue=%u",
        (unsigned)features.multiDrawIndirect, (unsigned)features.drawIndirectFirstInstance,
        (unsigned)features.fullDrawIndexUint32, properties.limits.maxDrawIndirectCount,
        properties.limits.maxDrawIndexedIndexValue);
    /* The witness runs only on a device that reports the three features and
     * the core floors they oblige; the device was created with the same three
     * bits enabled (main). */
    REQUIRE(features.multiDrawIndirect == VK_TRUE &&
            features.drawIndirectFirstInstance == VK_TRUE &&
            features.fullDrawIndexUint32 == VK_TRUE,
            "indirect and 32-bit index features reported");
    REQUIRE(properties.limits.maxDrawIndirectCount >= INDIRECT_MAX_COMMANDS &&
            properties.limits.maxDrawIndexedIndexValue == 0xffffffffu,
            "indirect count and index value floors reported");
    enum { CASE_COUNT = 10 };
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_INDIRECT_START cases=%u extent=%u clear_word=%08x",
                  (unsigned)CASE_COUNT, (unsigned)INDIRECT_EXTENT, INDIRECT_CLEAR_WORD);

    struct indirect_frame f = {.device = device, .queue = queue};

    /* Colour target with the exact shape the pinned upstream draw cases create. */
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {INDIRECT_EXTENT, INDIRECT_EXTENT, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(device, &image_info, NULL, &f.image));
    VkMemoryRequirements image_requirements;
    vkGetImageMemoryRequirements(device, f.image, &image_requirements);
    VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size, .memoryTypeIndex = 0};
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &image_allocation, NULL, &image_memory));
    CHECK(vkBindImageMemory(device, f.image, image_memory, 0));
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = f.image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view = VK_NULL_HANDLE;
    CHECK(vkCreateImageView(device, &view_info, NULL, &view));
    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color_reference};
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass};
    CHECK(vkCreateRenderPass(device, &pass_info, NULL, &f.pass));
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = f.pass, .attachmentCount = 1, .pAttachments = &view,
        .width = INDIRECT_EXTENT, .height = INDIRECT_EXTENT, .layers = 1};
    CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &f.framebuffer));

    /* Linear staging image for the readback. */
    VkImageCreateInfo staging_info = image_info;
    staging_info.tiling = VK_IMAGE_TILING_LINEAR;
    staging_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CHECK(vkCreateImage(device, &staging_info, NULL, &f.staging_image));
    VkMemoryRequirements staging_requirements;
    vkGetImageMemoryRequirements(device, f.staging_image, &staging_requirements);
    VkMemoryAllocateInfo staging_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = staging_requirements.size, .memoryTypeIndex = 0};
    CHECK(vkAllocateMemory(device, &staging_allocation, NULL, &f.staging_memory));
    CHECK(vkBindImageMemory(device, f.staging_image, f.staging_memory, 0));
    f.staging_bytes = staging_requirements.size;
    unsigned char *staging_bytes = NULL;
    CHECK(vkMapMemory(device, f.staging_memory, 0, f.staging_bytes, 0, (void **)&staging_bytes));
    REQUIRE(staging_bytes != NULL, "indirect witness staging map");
    f.staging_rows = staging_bytes;
    VkSubresourceLayout staging_layout = {0};
    const VkImageSubresource staging_subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    vkGetImageSubresourceLayout(device, f.staging_image, &staging_subresource, &staging_layout);
    f.staging_rows += staging_layout.offset;
    f.staging_pitch = staging_layout.rowPitch;
    REQUIRE(staging_layout.rowPitch >= INDIRECT_EXTENT * 4u &&
            staging_layout.offset + staging_layout.rowPitch * INDIRECT_EXTENT <= f.staging_bytes,
            "indirect witness staging layout");

    /* Pipeline: the witness vertex shader with its four push words. */
    VkShaderModuleCreateInfo vertex_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_indirect_witness_vert_spirv),
        .pCode = consumer_indirect_witness_vert_spirv};
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &vertex_info, NULL, &vertex_module));
    VkShaderModuleCreateInfo fragment_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_draw_parameters_frag_spirv),
        .pCode = consumer_draw_parameters_frag_spirv};
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &fragment_info, NULL, &fragment_module));
    VkPushConstantRange push_range = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(struct indirect_push)};
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range};
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &f.layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_module, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_module, .pName = "main"}};
    VkVertexInputBindingDescription binding = {
        .binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute = {
        .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkViewport viewport = {0.0f, 0.0f, INDIRECT_EXTENT, INDIRECT_EXTENT, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {INDIRECT_EXTENT, INDIRECT_EXTENT}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &input_assembly,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pViewportState = &viewport_state, .pColorBlendState = &blend,
        .layout = f.layout, .renderPass = f.pass};
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &f.pipeline));
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_INDIRECT_PIPELINE topology=triangle_list push_bytes=16 created=1");

    /* Buffers: zero-filled vertices, host-written indices, and the argument
     * buffer that also serves as the compute-written storage buffer. */
    VkBufferCreateInfo vertex_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = INDIRECT_VERTEX_BYTES,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device, &vertex_buffer_info, NULL, &f.vertex_buffer));
    VkMemoryRequirements vertex_requirements;
    vkGetBufferMemoryRequirements(device, f.vertex_buffer, &vertex_requirements);
    VkMemoryAllocateInfo vertex_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = vertex_requirements.size, .memoryTypeIndex = 0};
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &vertex_allocation, NULL, &vertex_memory));
    CHECK(vkBindBufferMemory(device, f.vertex_buffer, vertex_memory, 0));
    void *vertex_data = NULL;
    CHECK(vkMapMemory(device, vertex_memory, 0, vertex_requirements.size, 0, &vertex_data));
    memset(vertex_data, 0, (size_t)vertex_requirements.size);
    indirect_flush(device, vertex_memory);

    VkBufferCreateInfo index_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = INDIRECT_INDEX_BYTES,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device, &index_buffer_info, NULL, &f.index_buffer));
    VkMemoryRequirements index_requirements;
    vkGetBufferMemoryRequirements(device, f.index_buffer, &index_requirements);
    VkMemoryAllocateInfo index_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = index_requirements.size, .memoryTypeIndex = 0};
    CHECK(vkAllocateMemory(device, &index_allocation, NULL, &f.index_memory));
    CHECK(vkBindBufferMemory(device, f.index_buffer, f.index_memory, 0));
    CHECK(vkMapMemory(device, f.index_memory, 0, index_requirements.size, 0, (void **)&f.indices));

    VkBufferCreateInfo argument_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = INDIRECT_ARGUMENT_BYTES,
        .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device, &argument_buffer_info, NULL, &f.argument_buffer));
    VkMemoryRequirements argument_requirements;
    vkGetBufferMemoryRequirements(device, f.argument_buffer, &argument_requirements);
    VkMemoryAllocateInfo argument_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = argument_requirements.size, .memoryTypeIndex = 0};
    CHECK(vkAllocateMemory(device, &argument_allocation, NULL, &f.argument_memory));
    CHECK(vkBindBufferMemory(device, f.argument_buffer, f.argument_memory, 0));
    CHECK(vkMapMemory(device, f.argument_memory, 0, argument_requirements.size, 0,
                      (void **)&f.arguments));

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &command_info, &f.command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fence_info, NULL, &f.fence));

    /* The colour target enters GENERAL exactly the way the pinned upstream
     * draw cases and the draw-parameter witness do: an UNDEFINED -> GENERAL
     * transfer transition, a clear in GENERAL and the resource-less transfer
     * -> colour-attachment barrier, in one prelude submission. The render pass
     * below declares GENERAL for its initial and final layouts, so the tracked
     * layout must really be GENERAL before the first frame loads it. */
    {
        CHECK(vkResetCommandBuffer(f.command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(f.command, &begin));
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier to_general = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = f.image, .subresourceRange = range};
        vkCmdPipelineBarrier(f.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
        VkClearColorValue prelude_clear = {0};
        prelude_clear.float32[0] = prelude_clear.float32[1] = prelude_clear.float32[2] = 0x40 / 255.0f;
        prelude_clear.float32[3] = 1.0f;
        vkCmdClearColorImage(f.command, f.image, VK_IMAGE_LAYOUT_GENERAL, &prelude_clear, 1, &range);
        VkMemoryBarrier to_color = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        vkCmdPipelineBarrier(f.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &to_color, 0, NULL, 0, NULL);
        indirect_submit_and_wait(&f);
        ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_INDIRECT_TARGET layout=general clear_word=%08x prelude=1",
                      INDIRECT_CLEAR_WORD);
    }

    static uint32_t expected[INDIRECT_MAX_EXPECTED];
    unsigned witnessed = 0;
    const uint32_t junk[4] = {0xdeadbeefu, 0xdeadbeefu, 0xdeadbeefu, 0xdeadbeefu};

    /* 1. drawIndirectFirstInstance, non-indexed: one command at a non-trivial
     * offset draws three instances starting at instance 5; cell i is that
     * instance's ordinal and its colour carries gl_InstanceIndex = 5 + i. */
    {
        struct indirect_push push = {4, 1, 0, 0};
        const VkDrawIndirectCommand command = {3, 3, 0, 5};
        indirect_write_arguments(&f, 16, &command, sizeof(command));
        indirect_flush(device, f.argument_memory);
        indirect_begin_frame(&f, &push, VK_INDEX_TYPE_UINT16, 0);
        vkCmdDrawIndirect(f.command, f.argument_buffer, 16, 1, 0);
        indirect_end_frame(&f);
        for (unsigned i = 0; i < 3; ++i) expected[i] = indirect_word(0, 5 + i, 0);
        witnessed += indirect_judge(&f, "indirect_first_instance", 4, expected, 3);
    }
    /* 2. drawIndirectFirstInstance, indexed: uint16 indices 1..3 with
     * vertexOffset -1 (BaseVertex 0xff in the red byte) and firstInstance 200
     * over two instances. */
    {
        struct indirect_push push = {4, 1, 0, 0};
        const uint16_t indices[3] = {1, 2, 3};
        memcpy(f.indices, indices, sizeof(indices));
        indirect_flush(device, f.index_memory);
        const VkDrawIndexedIndirectCommand command = {3, 2, 0, -1, 200};
        indirect_write_arguments(&f, 32, &command, sizeof(command));
        indirect_flush(device, f.argument_memory);
        indirect_begin_frame(&f, &push, VK_INDEX_TYPE_UINT16, 1);
        vkCmdDrawIndexedIndirect(f.command, f.argument_buffer, 32, 1, 0);
        indirect_end_frame(&f);
        for (unsigned i = 0; i < 2; ++i) expected[i] = indirect_word(0xff, 200 + i, 0);
        witnessed += indirect_judge(&f, "indexed_indirect_first_instance", 4, expected, 2);
    }
    /* 3. multiDrawIndirect: four commands at a 32-byte stride from offset 64
     * with junk in the gaps; command 1 draws no vertex but keeps DrawIndex 1,
     * so cells 0, 2 and 3 carry DrawIndex k in blue and firstInstance 7 + k
     * in green while cell 1 stays clear. */
    {
        struct indirect_push push = {4, 0, 0, 0};
        for (unsigned k = 0; k < 4; ++k) {
            const VkDrawIndirectCommand command = {k == 1 ? 0u : 3u, 1, 0, 7 + k};
            indirect_write_arguments(&f, 64 + 32 * k, &command, sizeof(command));
            indirect_write_arguments(&f, 64 + 32 * k + 16, junk, sizeof(junk));
        }
        indirect_flush(device, f.argument_memory);
        indirect_begin_frame(&f, &push, VK_INDEX_TYPE_UINT16, 0);
        vkCmdDrawIndirect(f.command, f.argument_buffer, 64, 4, 32);
        indirect_end_frame(&f);
        for (unsigned k = 0; k < 4; ++k)
            expected[k] = k == 1 ? INDIRECT_CLEAR_WORD : indirect_word(0, 7 + k, k);
        witnessed += indirect_judge(&f, "multi_draw", 4, expected, 4);
    }
    /* 4. multiDrawIndirect, indexed and packed: four 20-byte commands with
     * vertexOffset -1 over uint16 indices 1..3; command 2 has no instance. */
    {
        struct indirect_push push = {4, 0, 0, 0};
        const uint16_t indices[3] = {1, 2, 3};
        memcpy(f.indices, indices, sizeof(indices));
        indirect_flush(device, f.index_memory);
        for (unsigned k = 0; k < 4; ++k) {
            const VkDrawIndexedIndirectCommand command = {3, k == 2 ? 0u : 1u, 0, -1, 11 + k};
            indirect_write_arguments(&f, 20 * k, &command, sizeof(command));
        }
        indirect_flush(device, f.argument_memory);
        indirect_begin_frame(&f, &push, VK_INDEX_TYPE_UINT16, 1);
        vkCmdDrawIndexedIndirect(f.command, f.argument_buffer, 0, 4, 20);
        indirect_end_frame(&f);
        for (unsigned k = 0; k < 4; ++k)
            expected[k] = k == 2 ? INDIRECT_CLEAR_WORD : indirect_word(0xff, 11 + k, k);
        witnessed += indirect_judge(&f, "multi_draw_indexed", 4, expected, 4);
    }
    /* 5. GPU-generated arguments: the host fills four commands that draw
     * nothing, a compute dispatch overwrites them with three-vertex commands
     * whose firstInstance is 20 + k, a compute -> indirect barrier orders
     * the write, and the multi-draw in the next submission consumes them. */
    {
        struct indirect_push push = {4, 0, 0, 0};
        for (unsigned k = 0; k < 4; ++k) {
            const VkDrawIndirectCommand nothing = {0, 1, 0, 0};
            indirect_write_arguments(&f, 16 * k, &nothing, sizeof(nothing));
        }
        indirect_flush(device, f.argument_memory);
        VkDescriptorSetLayoutBinding storage_binding = {
            .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo set_layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &storage_binding};
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout));
        VkPushConstantRange compute_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo compute_layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &set_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &compute_range};
        VkPipelineLayout compute_layout = VK_NULL_HANDLE;
        CHECK(vkCreatePipelineLayout(device, &compute_layout_info, NULL, &compute_layout));
        VkShaderModuleCreateInfo compute_module_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(consumer_indirect_arguments_comp_spirv),
            .pCode = consumer_indirect_arguments_comp_spirv};
        VkShaderModule compute_module = VK_NULL_HANDLE;
        CHECK(vkCreateShaderModule(device, &compute_module_info, NULL, &compute_module));
        VkComputePipelineCreateInfo compute_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = compute_module,
                      .pName = "main"},
            .layout = compute_layout};
        VkPipeline compute_pipeline = VK_NULL_HANDLE;
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_info, NULL,
                                       &compute_pipeline));
        VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo descriptor_pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        CHECK(vkCreateDescriptorPool(device, &descriptor_pool_info, NULL, &descriptor_pool));
        VkDescriptorSetAllocateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descriptor_pool, .descriptorSetCount = 1,
            .pSetLayouts = &set_layout};
        VkDescriptorSet set = VK_NULL_HANDLE;
        CHECK(vkAllocateDescriptorSets(device, &set_info, &set));
        VkDescriptorBufferInfo buffer_info = {f.argument_buffer, 0, 64};
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer_info};
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

        CHECK(vkResetCommandBuffer(f.command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(f.command, &begin));
        vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
        vkCmdBindDescriptorSets(f.command, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout,
                                0, 1, &set, 0, NULL);
        const uint32_t generate[4] = {4, 3, 20, 4};
        vkCmdPushConstants(f.command, compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(generate), generate);
        vkCmdDispatch(f.command, 1, 1, 1);
        VkMemoryBarrier to_indirect = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
        vkCmdPipelineBarrier(f.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &to_indirect, 0, NULL, 0, NULL);
        indirect_submit_and_wait(&f);
        ps5log_line(PS5LOG_MARK,
            "PS5VK_CONSUMER_INDIRECT_ARGUMENTS_GENERATED commands=4 vertices=3 first_instance_base=20 "
            "barrier=compute_shader_write_to_indirect_command_read");
        indirect_begin_frame(&f, &push, VK_INDEX_TYPE_UINT16, 0);
        vkCmdDrawIndirect(f.command, f.argument_buffer, 0, 4, 16);
        indirect_end_frame(&f);
        for (unsigned k = 0; k < 4; ++k) expected[k] = indirect_word(0, 20 + k, k);
        witnessed += indirect_judge(&f, "compute_generated_arguments", 4, expected, 4);
        /* The host reads the generated arguments back only to report them;
         * the judgement above rests on the pixels alone. */
        VkMappedMemoryRange arguments_invalidate = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = f.argument_memory, .offset = 0, .size = 64};
        CHECK(vkInvalidateMappedMemoryRanges(device, 1, &arguments_invalidate));
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_CONSUMER_INDIRECT_ARGUMENTS_READBACK first=%u,%u,%u,%u last=%u,%u,%u,%u",
            f.arguments[0], f.arguments[1], f.arguments[2], f.arguments[3],
            f.arguments[12], f.arguments[13], f.arguments[14], f.arguments[15]);
        vkDestroyDescriptorPool(device, descriptor_pool, NULL);
        vkDestroyPipeline(device, compute_pipeline, NULL);
        vkDestroyShaderModule(device, compute_module, NULL);
        vkDestroyPipelineLayout(device, compute_layout, NULL);
        vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    }
    /* 6. maxDrawIndirectCount: 65535 packed commands in one call, one pixel
     * per command; each pixel carries its own 16-bit DrawIndex and the marker,
     * and pixel 65535 - the only cell no command names - stays clear. */
    {
        struct indirect_push push = {256, 0, 1, 0x5a};
        for (unsigned k = 0; k < INDIRECT_MAX_COMMANDS; ++k) {
            f.arguments[4 * k + 0] = 3; f.arguments[4 * k + 1] = 1;
            f.arguments[4 * k + 2] = 0; f.arguments[4 * k + 3] = 0;
        }
        indirect_flush(device, f.argument_memory);
        indirect_begin_frame(&f, &push, VK_INDEX_TYPE_UINT16, 0);
        vkCmdDrawIndirect(f.command, f.argument_buffer, 0, INDIRECT_MAX_COMMANDS,
                          sizeof(VkDrawIndirectCommand));
        indirect_end_frame(&f);
        for (unsigned k = 0; k < INDIRECT_MAX_COMMANDS; ++k)
            expected[k] = indirect_word(k & 0xffu, k >> 8, 0x5a);
        expected[INDIRECT_MAX_COMMANDS] = INDIRECT_CLEAR_WORD;
        witnessed += indirect_judge(&f, "max_draw_indirect_count", 256, expected,
                                    INDIRECT_MAX_COMMANDS + 1);
    }
    /* 7-9. fullDrawIndexUint32: twelve indices per frame select cells 0..3
     * through gl_VertexIndex / 3, so the effective index (index + vertexOffset)
     * must land on 0..11 exactly. Bit 31 set with vertexOffset INT32_MIN, the
     * 2^24 boundary the 24-bit floor stops at, and a uint16 control. */
    {
        const struct {
            const char *name; VkIndexType type; uint32_t base; int32_t vertex_offset; uint32_t marker;
        } ranges[3] = {
            {"uint32_bit31_indices", VK_INDEX_TYPE_UINT32, 0x80000000u, INT32_MIN, 0x33},
            {"uint32_2pow24_indices", VK_INDEX_TYPE_UINT32, 0x01000000u, -16777216, 0x44},
            {"uint16_control_indices", VK_INDEX_TYPE_UINT16, 3u, -3, 0x55},
        };
        for (unsigned r = 0; r < 3; ++r) {
            struct indirect_push push = {4, 2, 1, ranges[r].marker};
            if (ranges[r].type == VK_INDEX_TYPE_UINT32) {
                uint32_t indices[12];
                for (unsigned j = 0; j < 12; ++j) indices[j] = ranges[r].base + j;
                memcpy(f.indices, indices, sizeof(indices));
            } else {
                uint16_t indices[12];
                for (unsigned j = 0; j < 12; ++j) indices[j] = (uint16_t)(ranges[r].base + j);
                memcpy(f.indices, indices, sizeof(indices));
            }
            indirect_flush(device, f.index_memory);
            indirect_begin_frame(&f, &push, ranges[r].type, 1);
            vkCmdDrawIndexed(f.command, 12, 1, 0, ranges[r].vertex_offset, 0);
            indirect_end_frame(&f);
            for (unsigned k = 0; k < 4; ++k) expected[k] = indirect_word(k, 0, ranges[r].marker);
            witnessed += indirect_judge(&f, ranges[r].name, 4, expected, 4);
        }
    }
    /* 10. The same bit-31 range through an indexed indirect command, so the
     * argument path carries the full uint32 indexCount/firstIndex words and
     * the signed INT32_MIN vertexOffset. */
    {
        struct indirect_push push = {4, 2, 1, 0x66};
        uint32_t indices[12];
        for (unsigned j = 0; j < 12; ++j) indices[j] = 0x80000000u + j;
        memcpy(f.indices, indices, sizeof(indices));
        indirect_flush(device, f.index_memory);
        const VkDrawIndexedIndirectCommand command = {12, 1, 0, INT32_MIN, 0};
        indirect_write_arguments(&f, 48, &command, sizeof(command));
        indirect_flush(device, f.argument_memory);
        indirect_begin_frame(&f, &push, VK_INDEX_TYPE_UINT32, 1);
        vkCmdDrawIndexedIndirect(f.command, f.argument_buffer, 48, 1, 0);
        indirect_end_frame(&f);
        for (unsigned k = 0; k < 4; ++k) expected[k] = indirect_word(k, 0, 0x66);
        witnessed += indirect_judge(&f, "uint32_bit31_indexed_indirect", 4, expected, 4);
    }

    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_INDIRECT_RESULT cases=%u witnessed=%u valid=%d",
                  (unsigned)CASE_COUNT, witnessed, witnessed == CASE_COUNT);

    vkDestroyFence(device, f.fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkUnmapMemory(device, f.argument_memory);
    vkDestroyBuffer(device, f.argument_buffer, NULL);
    vkFreeMemory(device, f.argument_memory, NULL);
    vkUnmapMemory(device, f.index_memory);
    vkDestroyBuffer(device, f.index_buffer, NULL);
    vkFreeMemory(device, f.index_memory, NULL);
    vkUnmapMemory(device, vertex_memory);
    vkDestroyBuffer(device, f.vertex_buffer, NULL);
    vkFreeMemory(device, vertex_memory, NULL);
    vkDestroyPipeline(device, f.pipeline, NULL);
    vkDestroyPipelineLayout(device, f.layout, NULL);
    vkDestroyShaderModule(device, fragment_module, NULL);
    vkDestroyShaderModule(device, vertex_module, NULL);
    vkDestroyFramebuffer(device, f.framebuffer, NULL);
    vkDestroyRenderPass(device, f.pass, NULL);
    vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, f.image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    vkUnmapMemory(device, f.staging_memory);
    vkDestroyImage(device, f.staging_image, NULL);
    vkFreeMemory(device, f.staging_memory, NULL);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_INDIRECT_RETIRED cases=%u witnessed=%u",
                  (unsigned)CASE_COUNT, witnessed);
}

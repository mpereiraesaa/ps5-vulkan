/* SPDX-License-Identifier: GPL-3.0-or-later
 * SDK-only diagnostic witness: one imageless framebuffer, two distinct views,
 * two completed colour clears and exact image-to-buffer readback.
 */
static void run_imageless_framebuffer_witness(VkDevice device, VkQueue queue)
{
    enum { WIDTH = 64, HEIGHT = 64, PIXELS = WIDTH * HEIGHT, BYTES = PIXELS * 4 };
    const uint8_t expected[2][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}};
    VkImage images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory image_memory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {WIDTH, HEIGHT, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(vkCreateImage(device, &image_info, NULL, &images[i]));
        VkMemoryRequirements requirements = {0};
        vkGetImageMemoryRequirements(device, images[i], &requirements);
        VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = 0};
        CHECK(vkAllocateMemory(device, &allocation, NULL, &image_memory[i]));
        CHECK(vkBindImageMemory(device, images[i], image_memory[i], 0));
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        CHECK(vkCreateImageView(device, &view_info, NULL, &views[i]));
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements buffer_requirements = {0};
    vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
    VkMemoryAllocateInfo buffer_allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_requirements.size, .memoryTypeIndex = 0};
    CHECK(vkAllocateMemory(device, &buffer_allocation, NULL, &buffer_memory));
    CHECK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
    uint8_t *readback = NULL;
    CHECK(vkMapMemory(device, buffer_memory, 0, buffer_requirements.size, 0,
                      (void **)&readback));

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass pass = VK_NULL_HANDLE;
    CHECK(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFramebufferAttachmentImageInfo image_contract = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO,
        .usage = image_info.usage, .width = WIDTH, .height = HEIGHT,
        .layerCount = 1, .viewFormatCount = 1, .pViewFormats = &format};
    VkFramebufferAttachmentsCreateInfo attachments = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO,
        .attachmentImageInfoCount = 1, .pAttachmentImageInfos = &image_contract};
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = &attachments, .flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT,
        .renderPass = pass, .attachmentCount = 1,
        .width = WIDTH, .height = HEIGHT, .layers = 1};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0};
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2};
    VkCommandBuffer commands[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    CHECK(vkAllocateCommandBuffers(device, &command_info, commands));
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fences[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    CHECK(vkCreateFence(device, &fence_info, NULL, &fences[0]));
    CHECK(vkCreateFence(device, &fence_info, NULL, &fences[1]));
    unsigned mismatches[2] = {0, 0};
    for (unsigned i = 0; i < 2; ++i) {
        VkCommandBuffer command = commands[i];
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkBeginCommandBuffer(command, &begin_info));
        VkRenderPassAttachmentBeginInfo begin_attachments = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO,
            .attachmentCount = 1, .pAttachments = &views[i]};
        VkClearValue clear = {.color = {.float32 = {
            expected[i][0] / 255.0f, expected[i][1] / 255.0f,
            expected[i][2] / 255.0f, 1.0f}}};
        VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = &begin_attachments, .renderPass = pass,
            .framebuffer = framebuffer, .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
            .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment color_clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0, .clearValue = clear};
        VkClearRect rect = {.rect = {{0, 0}, {WIDTH, HEIGHT}},
            .baseArrayLayer = 0, .layerCount = 1};
        vkCmdClearAttachments(command, 1, &color_clear, 1, &rect);
        vkCmdEndRenderPass(command);
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        /* Colour attachment readback uses the backend's whole-surface,
         * zero-offset shape. Check each completed view before reusing it. */
        VkBufferImageCopy copy = {.bufferOffset = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {WIDTH, HEIGHT, 1}};
        vkCmdCopyImageToBuffer(command, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            buffer, 1, &copy);
        VkBufferMemoryBarrier host_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer, .offset = 0, .size = BYTES};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host_barrier, 0, NULL);
        CHECK(vkEndCommandBuffer(command));
        CHECK(vkQueueSubmit(queue, 1, &submit, fences[i]));
        REQUIRE(vkWaitForFences(device, 1, &fences[i], VK_TRUE,
                                UINT64_C(5000000000)) == VK_SUCCESS,
                "imageless framebuffer clear completes within bounded fence");
        VkMappedMemoryRange invalidate = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = buffer_memory, .offset = 0, .size = VK_WHOLE_SIZE};
        CHECK(vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
        for (unsigned pixel = 0; pixel < PIXELS; ++pixel)
            for (unsigned channel = 0; channel < 4; ++channel)
                mismatches[i] += readback[pixel * 4 + channel] != expected[i][channel];
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_IMAGELESS_RESULT views=2 same_framebuffer=1 pixels=%u"
        " first_mismatches=%u second_mismatches=%u valid=%u",
        PIXELS, mismatches[0], mismatches[1],
        !mismatches[0] && !mismatches[1]);
    REQUIRE(!mismatches[0] && !mismatches[1], "imageless two-view readback");

    vkDestroyFence(device, fences[0], NULL);
    vkDestroyFence(device, fences[1], NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    vkUnmapMemory(device, buffer_memory);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, buffer_memory, NULL);
    for (unsigned i = 0; i < 2; ++i) {
        vkDestroyImageView(device, views[i], NULL);
        vkDestroyImage(device, images[i], NULL);
        vkFreeMemory(device, image_memory[i], NULL);
    }
}

#ifndef PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_WITNESS_H
#define PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_WITNESS_H

/* Include after CHECK, REQUIRE, and the generated UBO shader header. The
 * caller must have enabled VK_KHR_uniform_buffer_standard_layout. */
static void run_ubo_standard_layout_witness(VkDevice device, VkQueue queue)
{
    enum { BUFFER_BYTES = 1024, UBO_BYTES = 136, OUTPUT_OFFSET = 256,
           OUTPUT_COUNT = 64, OUTPUT_BYTES = OUTPUT_COUNT * 4 };
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_START");
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_ubo_standard_layout_spirv),
        .pCode = consumer_ubo_standard_layout_spirv,
    };
    CHECK(vkCreateShaderModule(device, &module_info, NULL, &module));
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings,
    };
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
    };
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout));
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module,
                  .pName = "main"},
        .layout = pipeline_layout,
    };
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                   &pipeline_info, NULL, &pipeline));

    for (unsigned i = 0; i < 2; ++i) {
        VkBufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BUFFER_BYTES,
            .usage = i ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT :
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        };
        CHECK(vkCreateBuffer(device, &info, NULL, &buffers[i]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = 0,
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memory[i]));
        CHECK(vkBindBufferMemory(device, buffers[i], memory[i], 0));
        uint8_t *mapped = NULL;
        CHECK(vkMapMemory(device, memory[i], 0, BUFFER_BYTES, 0,
                          (void **)&mapped));
        memset(mapped, 0xa5, BUFFER_BYTES);
        if (!i) {
            uint32_t word;
            for (unsigned index = 0; index < 8; ++index) {
                word = 0x10100100u + index * 0x10101u;
                memcpy(mapped + index * 4, &word, 4);
            }
            for (unsigned index = 0; index < 4; ++index) {
                word = 0x3f800000u + index * 0x00800000u;
                memcpy(mapped + 32 + index * 8, &word, 4);
            }
            word = 0x40a00000u; /* matrix[0][0] = 5.0 at byte 64 */
            memcpy(mapped + 64, &word, 4);
            word = 0x41100000u; /* matrix[1][2] = 9.0 at byte 84 */
            memcpy(mapped + 84, &word, 4);
            for (unsigned index = 0; index < 2; ++index) {
                word = 0x41800000u + index * 0x00800000u;
                memcpy(mapped + 100 + index * 24, &word, 4);
                word = 0xc3010101u + index * 0x1010101u;
                memcpy(mapped + 104 + index * 24, &word, 4);
            }
        }
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory[i], .offset = 0, .size = BUFFER_BYTES,
        };
        CHECK(vkFlushMappedMemoryRanges(device, 1, &range));
        vkUnmapMemory(device, memory[i]);
    }

    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1},
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes,
    };
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo allocate_set = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    CHECK(vkAllocateDescriptorSets(device, &allocate_set, &descriptor_set));
    VkDescriptorBufferInfo descriptor_info[2] = {
        {.buffer = buffers[0], .offset = 0, .range = UBO_BYTES},
        {.buffer = buffers[1], .offset = OUTPUT_OFFSET, .range = OUTPUT_BYTES},
    };
    VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo = &descriptor_info[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &descriptor_info[1]},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command, 2, 1, 1);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(300000000)));

    uint8_t *mapped = NULL;
    CHECK(vkMapMemory(device, memory[1], 0, BUFFER_BYTES, 0, (void **)&mapped));
    VkMappedMemoryRange invalidate = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory[1], .offset = 0, .size = BUFFER_BYTES,
    };
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
    unsigned mismatches = 0, guard_mismatches = 0;
    for (unsigned i = 0; i < BUFFER_BYTES; ++i) {
        if ((i < OUTPUT_OFFSET || i >= OUTPUT_OFFSET + OUTPUT_BYTES) &&
            mapped[i] != 0xa5) ++guard_mismatches;
    }
    for (unsigned i = 0; i < OUTPUT_COUNT; ++i) {
        unsigned index = i & 7u;
        uint32_t actual;
        memcpy(&actual, mapped + OUTPUT_OFFSET + i * 4, 4);
        uint32_t expected = (0x10100100u + index * 0x10101u)
            ^ (0x3f800000u + (index & 3u) * 0x00800000u)
            ^ 0x40a00000u ^ 0x41100000u
            ^ (0x41800000u + (index & 1u) * 0x00800000u)
            ^ (0xc3010101u + (index & 1u) * 0x1010101u)
            ^ (i * 0x9e3779b9u);
        if (actual != expected) ++mismatches;
    }
    vkUnmapMemory(device, memory[1]);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_RESULT workgroups=2 values=64 "
        "ubo_bytes=%u mismatches=%u guard_mismatches=%u",
        UBO_BYTES, mismatches, guard_mismatches);
    REQUIRE(!mismatches && !guard_mismatches,
            "compact UBO GPU values and output guards");

    vkDestroyFence(device, fence, NULL);
    vkFreeCommandBuffers(device, command_pool, 1, &command);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    for (unsigned i = 0; i < 2; ++i) {
        vkDestroyBuffer(device, buffers[i], NULL);
        vkFreeMemory(device, memory[i], NULL);
    }
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, module, NULL);
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_RETIRED");
}

#endif

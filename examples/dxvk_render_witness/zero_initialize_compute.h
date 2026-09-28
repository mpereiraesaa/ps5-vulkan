/* SPDX-License-Identifier: GPL-3.0-or-later
 * SDK-only workgroup-null execution; host tests do not prove GPU semantics. */
#ifndef PS5VK_ZERO_INITIALIZE_COMPUTE_H
#define PS5VK_ZERO_INITIALIZE_COMPUTE_H

enum { ZERO_WORDS = 128, ZERO_GUARD = 64, ZERO_BYTES = ZERO_GUARD * 2 + ZERO_WORDS * 4 };
struct zero_initialize_result {
    uint32_t outputs, mismatches, guards, digest;
    const char *step;
};

static void zero_initialize_check(const unsigned char *mapped,
    struct zero_initialize_result *out)
{
    out->outputs = ZERO_WORDS;
    out->digest = UINT32_C(2166136261);
    for (uint32_t i = 0; i < ZERO_WORDS; ++i) {
        uint32_t observed;
        memcpy(&observed, mapped + ZERO_GUARD + i * 4u, 4);
        out->mismatches += observed != (i % 64u ? 123u : 246u);
        for (unsigned byte = 0; byte < 4; ++byte)
            out->digest = (out->digest ^ mapped[ZERO_GUARD + i * 4u + byte]) * UINT32_C(16777619);
    }
    for (uint32_t i = 0; i < ZERO_BYTES; ++i)
        if (i < ZERO_GUARD || i >= ZERO_GUARD + ZERO_WORDS * 4u)
            out->guards += mapped[i] != 0xcd;
}

static VkResult zero_initialize_compute_witness(VkDevice device, VkQueue queue,
    const uint32_t *words, size_t bytes, VkBool32 *pending,
    struct zero_initialize_result *out)
{
    VkResult result = VK_SUCCESS;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    unsigned char *mapped = NULL;
    *pending = VK_FALSE;
    memset(out, 0, sizeof(*out));
#define ZERO_TRY(call) do { out->step = #call; result = (call); if (result != VK_SUCCESS) goto cleanup; } while (0)
#define ZERO_REQUIRE(ok) do { out->step = #ok; if (!(ok)) { result = VK_ERROR_UNKNOWN; goto cleanup; } } while (0)
    VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = words};
    ZERO_TRY(vkCreateShaderModule(device, &sm, NULL, &module));
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo sl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    ZERO_TRY(vkCreateDescriptorSetLayout(device, &sl, NULL, &set_layout));
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    ZERO_TRY(vkCreatePipelineLayout(device, &pl, NULL, &layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = layout, .basePipelineIndex = -1};
    ZERO_TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ZERO_BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    ZERO_TRY(vkCreateBuffer(device, &bi, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    ZERO_REQUIRE(requirements.memoryTypeBits & 1u);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    ZERO_TRY(vkAllocateMemory(device, &ai, NULL, &memory));
    ZERO_TRY(vkBindBufferMemory(device, buffer, memory, 0));
    ZERO_TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    memset(mapped, 0xcd, ZERO_BYTES);
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .size = VK_WHOLE_SIZE};
    ZERO_TRY(vkFlushMappedMemoryRanges(device, 1, &range));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    ZERO_TRY(vkCreateDescriptorPool(device, &dpi, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set = VK_NULL_HANDLE;
    ZERO_TRY(vkAllocateDescriptorSets(device, &dsa, &set));
    VkDescriptorBufferInfo dbi = {buffer, ZERO_GUARD, ZERO_WORDS * 4u};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ZERO_TRY(vkCreateCommandPool(device, &cpi, NULL, &command_pool));
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VkCommandBuffer command = VK_NULL_HANDLE;
    ZERO_TRY(vkAllocateCommandBuffers(device, &cba, &command));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ZERO_TRY(vkCreateFence(device, &fi, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    ZERO_TRY(vkBeginCommandBuffer(command, &begin));
    VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &before, 0, NULL, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(command, 2, 1, 1);
    VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &after, 0, NULL, 0, NULL);
    ZERO_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    *pending = VK_TRUE;
    ZERO_TRY(vkQueueSubmit(queue, 1, &submit, fence));
    ZERO_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(300000000)));
    *pending = VK_FALSE;
    ZERO_TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
    zero_initialize_check(mapped, out);
    ZERO_REQUIRE(!out->mismatches && !out->guards);
cleanup:
    if (*pending) return result;
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (mapped) vkUnmapMemory(device, memory);
    if (buffer) vkDestroyBuffer(device, buffer, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (module) vkDestroyShaderModule(device, module, NULL);
    return result;
#undef ZERO_TRY
#undef ZERO_REQUIRE
}
#endif

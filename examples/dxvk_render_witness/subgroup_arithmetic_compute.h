/* SPDX-License-Identifier: GPL-3.0-or-later
 * SDK-only compute arithmetic census. Host checks do not prove GPU results. */
#ifndef PS5VK_SUBGROUP_ARITHMETIC_COMPUTE_H
#define PS5VK_SUBGROUP_ARITHMETIC_COMPUTE_H

enum { ARITH_LANES = 128, ARITH_OPS = 21, ARITH_WORDS = ARITH_LANES * ARITH_OPS,
       ARITH_GUARD = 16, ARITH_TOTAL = ARITH_WORDS + ARITH_GUARD * 2 };
struct subgroup_arithmetic_result {
    uint32_t outputs, mismatches, guards, digest;
    const char *step;
};

static uint32_t arithmetic_expected(uint32_t lane, uint32_t operation)
{
    const uint32_t kind = operation % 7u, mode = operation / 7u;
    const uint32_t end = mode == 0u ? 32u : mode == 1u ? lane + 1u : lane;
    uint32_t value = kind == 1u ? 1u :
        (kind == 2u || kind == 4u) ? UINT32_MAX : 0u;
    for (uint32_t i = 0; i < end; ++i) {
        const uint32_t next = (i & 1u) + 1u;
        switch (kind) {
        case 0u: value += next; break;
        case 1u: value *= next; break;
        case 2u: if (next < value) value = next; break;
        case 3u: if (next > value) value = next; break;
        case 4u: value &= next; break;
        case 5u: value |= next; break;
        default: value ^= next; break;
        }
    }
    return value;
}

static void arithmetic_check(const uint32_t *mapped,
    struct subgroup_arithmetic_result *out)
{
    out->outputs = ARITH_WORDS;
    out->digest = UINT32_C(2166136261);
    for (uint32_t i = 0; i < ARITH_GUARD; ++i) {
        out->guards += mapped[i] != UINT32_C(0xcdcdcdcd);
        out->guards += mapped[ARITH_GUARD + ARITH_WORDS + i] != UINT32_C(0xcdcdcdcd);
    }
    for (uint32_t lane = 0; lane < ARITH_LANES; ++lane)
        for (uint32_t op = 0; op < ARITH_OPS; ++op) {
            const uint32_t value = mapped[ARITH_GUARD + lane * ARITH_OPS + op];
            out->mismatches += value != arithmetic_expected(lane % 32u, op);
            out->digest = (out->digest ^ value) * UINT32_C(16777619);
        }
}

static VkResult subgroup_arithmetic_compute_witness(VkDevice device, VkQueue queue,
    const uint32_t *shader, size_t bytes, VkBool32 *pending,
    struct subgroup_arithmetic_result *out)
{
    VkResult result = VK_SUCCESS;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t *mapped = NULL;
    *pending = VK_FALSE;
    memset(out, 0, sizeof(*out));
#define ARITH_TRY(call) do { out->step = #call; result = (call); if (result != VK_SUCCESS) goto cleanup; } while (0)
#define ARITH_REQUIRE(ok) do { out->step = #ok; if (!(ok)) { result = VK_ERROR_UNKNOWN; goto cleanup; } } while (0)
    VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = shader};
    ARITH_TRY(vkCreateShaderModule(device, &sm, NULL, &module));
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo sl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    ARITH_TRY(vkCreateDescriptorSetLayout(device, &sl, NULL, &set_layout));
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    ARITH_TRY(vkCreatePipelineLayout(device, &pl, NULL, &layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = layout, .basePipelineIndex = -1};
    ARITH_TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ARITH_TOTAL * sizeof(uint32_t), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    ARITH_TRY(vkCreateBuffer(device, &bi, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    ARITH_REQUIRE(requirements.memoryTypeBits & 1u);
    ARITH_REQUIRE(requirements.size >= ARITH_TOTAL * sizeof(uint32_t));
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    ARITH_TRY(vkAllocateMemory(device, &ai, NULL, &memory));
    ARITH_TRY(vkBindBufferMemory(device, buffer, memory, 0));
    ARITH_TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    memset(mapped, 0xcd, ARITH_TOTAL * sizeof(uint32_t));
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .size = VK_WHOLE_SIZE};
    ARITH_TRY(vkFlushMappedMemoryRanges(device, 1, &range));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    ARITH_TRY(vkCreateDescriptorPool(device, &dpi, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set = VK_NULL_HANDLE;
    ARITH_TRY(vkAllocateDescriptorSets(device, &dsa, &set));
    VkDescriptorBufferInfo dbi = {buffer, ARITH_GUARD * sizeof(uint32_t),
        ARITH_WORDS * sizeof(uint32_t)};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ARITH_TRY(vkCreateCommandPool(device, &cpi, NULL, &command_pool));
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VkCommandBuffer command = VK_NULL_HANDLE;
    ARITH_TRY(vkAllocateCommandBuffers(device, &cba, &command));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ARITH_TRY(vkCreateFence(device, &fi, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    ARITH_TRY(vkBeginCommandBuffer(command, &begin));
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
    ARITH_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    *pending = VK_TRUE;
    ARITH_TRY(vkQueueSubmit(queue, 1, &submit, fence));
    ARITH_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(300000000)));
    *pending = VK_FALSE;
    ARITH_TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
    arithmetic_check(mapped, out);
    ARITH_REQUIRE(!out->mismatches && !out->guards);
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
#undef ARITH_TRY
#undef ARITH_REQUIRE
}
#endif

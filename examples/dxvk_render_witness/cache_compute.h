/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public Vulkan-only cache execution sequence, shared with the host contract.
 * The host backend validates recording and the oracle, never GPU execution. */
#ifndef PS5VK_CACHE_COMPUTE_WITNESS_H
#define PS5VK_CACHE_COMPUTE_WITNESS_H

enum { CACHE_WORDS = 1024, CACHE_INPUT = 256, CACHE_OUTPUT = 4608,
       CACHE_BYTES = 8960 };
struct cache_compute_result { uint32_t digest, mismatches, guards, inputs; const char *step; };

static VkResult cache_compute_witness(VkDevice device, VkQueue queue,
    const uint32_t *words, size_t bytes, VkBool32 *pending,
    struct cache_compute_result *observed)
{
    VkResult result = VK_SUCCESS;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkPipeline base = VK_NULL_HANDLE, warm = VK_NULL_HANDLE, child = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    unsigned char *mapped = NULL;
    *pending = VK_FALSE;
    memset(observed, 0, sizeof(*observed));
#define CACHE_TRY(call) do { observed->step = #call; result = (call); if (result != VK_SUCCESS) goto cleanup; } while (0)
#define CACHE_REQUIRE(condition) do { observed->step = #condition; if (!(condition)) { result = VK_ERROR_UNKNOWN; goto cleanup; } } while (0)
    VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = words};
    CACHE_TRY(vkCreateShaderModule(device, &sm, NULL, &module));
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo sl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    CACHE_TRY(vkCreateDescriptorSetLayout(device, &sl, NULL, &set_layout));
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    CACHE_TRY(vkCreatePipelineLayout(device, &pl, NULL, &layout));
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT};
    CACHE_TRY(vkCreatePipelineCache(device, &pc, NULL, &cache));
    uint32_t factor = 3;
    VkSpecializationMapEntry entry = {.constantID = 0, .size = sizeof(factor)};
    VkSpecializationInfo specialization = {.mapEntryCount = 1, .pMapEntries = &entry,
        .dataSize = sizeof(factor), .pData = &factor};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
            .pSpecializationInfo = &specialization}, .layout = layout, .basePipelineIndex = -1};
    result = vkCreateComputePipelines(device, cache, 1, &ci, NULL, &base);
    CACHE_REQUIRE(result == VK_PIPELINE_COMPILE_REQUIRED && !base);
    ci.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
    CACHE_TRY(vkCreateComputePipelines(device, cache, 1, &ci, NULL, &base));
    factor = 5;
    ci.basePipelineHandle = base;
    ci.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    result = vkCreateComputePipelines(device, cache, 1, &ci, NULL, &child);
    CACHE_REQUIRE(result == VK_PIPELINE_COMPILE_REQUIRED && !child);
    ci.flags = 0; ci.basePipelineHandle = VK_NULL_HANDLE;
    CACHE_TRY(vkCreateComputePipelines(device, cache, 1, &ci, NULL, &warm));
    ci.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    ci.basePipelineHandle = base;
    CACHE_TRY(vkCreateComputePipelines(device, cache, 1, &ci, NULL, &child));
    vkDestroyPipeline(device, base, NULL); base = VK_NULL_HANDLE;
    vkDestroyPipeline(device, warm, NULL); warm = VK_NULL_HANDLE;
    vkDestroyShaderModule(device, module, NULL); module = VK_NULL_HANDLE;
    vkDestroyPipelineCache(device, cache, NULL); cache = VK_NULL_HANDLE;

    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = CACHE_BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    CACHE_TRY(vkCreateBuffer(device, &bi, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    CACHE_REQUIRE(requirements.memoryTypeBits & 1u);
    CACHE_TRY(vkAllocateMemory(device, &ai, NULL, &memory));
    CACHE_TRY(vkBindBufferMemory(device, buffer, memory, 0));
    CACHE_TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    memset(mapped, 0xcd, CACHE_BYTES);
    for (uint32_t i = 0; i < CACHE_WORDS; ++i) {
        uint32_t value = i * 17u + 11u;
        memcpy(mapped + CACHE_INPUT + i * 4u, &value, 4);
    }
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .size = VK_WHOLE_SIZE};
    CACHE_TRY(vkFlushMappedMemoryRanges(device, 1, &range));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    CACHE_TRY(vkCreateDescriptorPool(device, &dpi, NULL, &descriptors));
    VkDescriptorSetAllocateInfo ds = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    CACHE_TRY(vkAllocateDescriptorSets(device, &ds, &set));
    VkDescriptorBufferInfo buffers[2] = {{buffer, CACHE_INPUT, CACHE_WORDS * 4},
        {buffer, CACHE_OUTPUT, CACHE_WORDS * 4}};
    VkWriteDescriptorSet writes[2] = {0};
    for (uint32_t i = 0; i < 2; ++i) writes[i] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    CACHE_TRY(vkCreateCommandPool(device, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CACHE_TRY(vkAllocateCommandBuffers(device, &ca, &command));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CACHE_TRY(vkCreateFence(device, &fi, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CACHE_TRY(vkBeginCommandBuffer(command, &begin));
    VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &before, 0, NULL, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, child);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(command, CACHE_WORDS / 64, 1, 1);
    VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &after, 0, NULL, 0, NULL);
    CACHE_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    *pending = VK_TRUE;
    CACHE_TRY(vkQueueSubmit(queue, 1, &submit, fence));
    CACHE_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(300000000)));
    *pending = VK_FALSE;
    CACHE_TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
    observed->digest = UINT32_C(2166136261);
    for (uint32_t i = 0; i < CACHE_WORDS; ++i) {
        uint32_t input, output;
        memcpy(&input, mapped + CACHE_INPUT + i * 4u, 4);
        memcpy(&output, mapped + CACHE_OUTPUT + i * 4u, 4);
        observed->inputs += input != i * 17u + 11u;
        observed->mismatches += output != (i * 17u + 11u) * 5u + (i ^ UINT32_C(0x13579bdf));
    }
    for (uint32_t i = 0; i < CACHE_BYTES; ++i) {
        if (i >= CACHE_OUTPUT && i < CACHE_OUTPUT + CACHE_WORDS * 4)
            observed->digest = (observed->digest ^ mapped[i]) * UINT32_C(16777619);
        else if (!(i >= CACHE_INPUT && i < CACHE_INPUT + CACHE_WORDS * 4))
            observed->guards += mapped[i] != 0xcd;
    }
    CACHE_REQUIRE(!observed->inputs && !observed->mismatches && !observed->guards);
cleanup:
    /* The caller retains the device too when GPU ownership is unresolved. */
    if (*pending) return result;
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (descriptors) vkDestroyDescriptorPool(device, descriptors, NULL);
    if (mapped) vkUnmapMemory(device, memory);
    if (buffer) vkDestroyBuffer(device, buffer, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    if (child) vkDestroyPipeline(device, child, NULL);
    if (warm) vkDestroyPipeline(device, warm, NULL);
    if (base) vkDestroyPipeline(device, base, NULL);
    if (cache) vkDestroyPipelineCache(device, cache, NULL);
    if (module) vkDestroyShaderModule(device, module, NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    return result;
#undef CACHE_TRY
#undef CACHE_REQUIRE
}
#endif

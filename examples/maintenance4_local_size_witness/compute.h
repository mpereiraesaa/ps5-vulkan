/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5VK_MAINTENANCE4_LOCAL_SIZE_COMPUTE_H
#define PS5VK_MAINTENANCE4_LOCAL_SIZE_COMPUTE_H

enum { ML_GROUPS = 2, ML_GUARD = 128 };
struct ml_result { uint32_t outputs, mismatches, guards, digest; const char *step; };

static VkResult ml_compute(VkDevice device, VkQueue queue, const uint32_t *code,
    size_t code_bytes, uint32_t local_x, VkBool32 specialize, VkBool32 *pending,
    struct ml_result *observed)
{
    VkResult result = VK_SUCCESS;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
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
#define ML_TRY(call) do { observed->step = #call; result = (call); if (result != VK_SUCCESS) goto cleanup; } while (0)
#define ML_REQUIRE(condition) do { observed->step = #condition; if (!(condition)) { result = VK_ERROR_UNKNOWN; goto cleanup; } } while (0)
    ML_REQUIRE(code && code_bytes && (local_x == 32u || local_x == 64u));
    const uint32_t output_words = ML_GROUPS * local_x;
    const VkDeviceSize output_bytes = (VkDeviceSize)output_words * 4u;
    const VkDeviceSize buffer_bytes = output_bytes + 2u * ML_GUARD;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo sl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    ML_TRY(vkCreateDescriptorSetLayout(device, &sl, NULL, &set_layout));
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    ML_TRY(vkCreatePipelineLayout(device, &pl, NULL, &layout));
    VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_bytes, .pCode = code};
    ML_TRY(vkCreateShaderModule(device, &sm, NULL, &module));
    VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = 4};
    VkSpecializationInfo spec = {.mapEntryCount = 1, .pMapEntries = &entry,
        .dataSize = 4, .pData = &local_x};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
            .pSpecializationInfo = specialize ? &spec : NULL},
        .layout = layout, .basePipelineIndex = -1};
    ML_TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    vkDestroyShaderModule(device, module, NULL); module = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    ML_TRY(vkCreateBuffer(device, &bi, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    ML_REQUIRE(requirements.memoryTypeBits & 1u);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    ML_TRY(vkAllocateMemory(device, &ai, NULL, &memory));
    ML_TRY(vkBindBufferMemory(device, buffer, memory, 0));
    ML_TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    memset(mapped, 0xcd, (size_t)buffer_bytes);
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .size = VK_WHOLE_SIZE};
    ML_TRY(vkFlushMappedMemoryRanges(device, 1, &range));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    ML_TRY(vkCreateDescriptorPool(device, &dpi, NULL, &descriptors));
    VkDescriptorSetAllocateInfo ds = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    ML_TRY(vkAllocateDescriptorSets(device, &ds, &set));
    VkDescriptorBufferInfo output = {buffer, ML_GUARD, output_bytes};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &output};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ML_TRY(vkCreateCommandPool(device, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    ML_TRY(vkAllocateCommandBuffers(device, &ca, &command));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ML_TRY(vkCreateFence(device, &fi, NULL, &fence));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    ML_TRY(vkBeginCommandBuffer(command, &begin));
    VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &before, 0, NULL, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(command, ML_GROUPS, 1, 1);
    VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &after, 0, NULL, 0, NULL);
    ML_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    *pending = VK_TRUE;
    ML_TRY(vkQueueSubmit(queue, 1, &submit, fence));
    ML_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(300000000)));
    *pending = VK_FALSE;
    ML_TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
    observed->outputs = output_words;
    observed->digest = UINT32_C(2166136261);
    for (uint32_t i = 0; i < output_words; ++i) {
        uint32_t actual;
        memcpy(&actual, mapped + ML_GUARD + 4u * i, 4);
        uint32_t expected = UINT32_C(0x62000000) | ((i / local_x) << 16) | (i % local_x);
        observed->mismatches += actual != expected;
        observed->digest = (observed->digest ^ actual) * UINT32_C(16777619);
    }
    for (uint32_t i = 0; i < ML_GUARD; ++i) {
        observed->guards += mapped[i] != 0xcd;
        observed->guards += mapped[ML_GUARD + output_bytes + i] != 0xcd;
    }
    ML_REQUIRE(!observed->mismatches && !observed->guards);
cleanup:
    if (*pending) return result;
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (descriptors) vkDestroyDescriptorPool(device, descriptors, NULL);
    if (mapped) vkUnmapMemory(device, memory);
    if (buffer) vkDestroyBuffer(device, buffer, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (module) vkDestroyShaderModule(device, module, NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    return result;
#undef ML_TRY
#undef ML_REQUIRE
}
#endif

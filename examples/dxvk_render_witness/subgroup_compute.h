/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public Vulkan subgroup occupancy witness. A host backend can validate the
 * recording and oracle, but only native execution measures active GPU lanes. */
#ifndef PS5VK_SUBGROUP_COMPUTE_WITNESS_H
#define PS5VK_SUBGROUP_COMPUTE_WITNESS_H

enum { SUBGROUP_FIELDS = 8, SUBGROUP_GROUPS = 2, SUBGROUP_GUARD = 256 };
struct subgroup_compute_case {
    uint32_t dimensions[3];
    VkPipelineShaderStageCreateFlags flags;
    VkBool32 required_size;
};
struct subgroup_compute_result {
    uint32_t outputs, digest, mismatches, guards;
    const char *step;
};

/* Independent CPU oracle for the fixed-wave32 execution contract. Partial
 * groups exercise the instrument without REQUIRE_FULL_SUBGROUPS. */
static uint32_t subgroup_expected_word(uint32_t total, uint32_t invocation, uint32_t field)
{
    uint32_t local=invocation%total, group=local/32, active=total-group*32;
    if (active>32) active=32;
    switch (field) {
    case 0: return 32;
    case 1: return local%32;
    case 2: return group;
    case 3: return (total+31)/32;
    case 4: return active;
    case 5: return active==32 ? UINT32_MAX : (UINT32_C(1)<<active)-1;
    case 6: return 1;
    default: return local;
    }
}

static VkResult subgroup_compute_witness(VkDevice device, VkQueue queue,
    const uint32_t *words, size_t bytes, const struct subgroup_compute_case *config,
    VkBool32 *pending, struct subgroup_compute_result *observed)
{
    VkResult result=VK_SUCCESS;
    VkShaderModule module=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    unsigned char *mapped=NULL;
    *pending=VK_FALSE;
    memset(observed,0,sizeof(*observed));
#define SUBGROUP_TRY(call) do { observed->step=#call; result=(call); if(result!=VK_SUCCESS) goto cleanup; } while(0)
#define SUBGROUP_REQUIRE(condition) do { observed->step=#condition; if(!(condition)) { result=VK_ERROR_UNKNOWN; goto cleanup; } } while(0)
    SUBGROUP_REQUIRE(config && words && bytes);
    uint32_t total=1;
    for (unsigned i=0;i<3;++i) {
        SUBGROUP_REQUIRE(config->dimensions[i] && config->dimensions[i]<=1024/total);
        total*=config->dimensions[i];
    }
    const uint32_t output_words=SUBGROUP_GROUPS*total*SUBGROUP_FIELDS;
    const uint32_t output_bytes=output_words*4;
    const uint32_t buffer_bytes=output_bytes+2*SUBGROUP_GUARD;
    VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    SUBGROUP_TRY(vkCreateDescriptorSetLayout(device,&sl,NULL,&set_layout));
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&set_layout};
    SUBGROUP_TRY(vkCreatePipelineLayout(device,&pl,NULL,&layout));
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=bytes,.pCode=words};
    SUBGROUP_TRY(vkCreateShaderModule(device,&sm,NULL,&module));
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,.requiredSubgroupSize=32};
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext=config->required_size?&required:NULL,.flags=config->flags,
            .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},
        .layout=layout,.basePipelineIndex=-1};
    SUBGROUP_TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    vkDestroyShaderModule(device,module,NULL);module=VK_NULL_HANDLE;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=buffer_bytes,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    SUBGROUP_TRY(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device,buffer,&requirements);
    SUBGROUP_REQUIRE(requirements.memoryTypeBits & 1u);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=requirements.size,.memoryTypeIndex=0};
    SUBGROUP_TRY(vkAllocateMemory(device,&ai,NULL,&memory));
    SUBGROUP_TRY(vkBindBufferMemory(device,buffer,memory,0));
    SUBGROUP_TRY(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,(void**)&mapped));
    memset(mapped,0xcd,buffer_bytes);
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.size=VK_WHOLE_SIZE};
    SUBGROUP_TRY(vkFlushMappedMemoryRanges(device,1,&range));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    SUBGROUP_TRY(vkCreateDescriptorPool(device,&dpi,NULL,&descriptors));
    VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;
    SUBGROUP_TRY(vkAllocateDescriptorSets(device,&ds,&set));
    VkDescriptorBufferInfo output={buffer,SUBGROUP_GUARD,output_bytes};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&output};
    vkUpdateDescriptorSets(device,1,&write,0,NULL);
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    SUBGROUP_TRY(vkCreateCommandPool(device,&cpi,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;
    SUBGROUP_TRY(vkAllocateCommandBuffers(device,&ca,&command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    SUBGROUP_TRY(vkCreateFence(device,&fi,NULL,&fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    SUBGROUP_TRY(vkBeginCommandBuffer(command,&begin));
    VkMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,1,&before,0,NULL,0,NULL);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);
    vkCmdDispatch(command,SUBGROUP_GROUPS,1,1);
    VkMemoryBarrier after={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&after,0,NULL,0,NULL);
    SUBGROUP_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    *pending=VK_TRUE;
    SUBGROUP_TRY(vkQueueSubmit(queue,1,&submit,fence));
    SUBGROUP_TRY(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(300000000)));
    *pending=VK_FALSE;
    SUBGROUP_TRY(vkInvalidateMappedMemoryRanges(device,1,&range));
    observed->outputs=output_words;
    observed->digest=UINT32_C(2166136261);
    for (uint32_t i=0;i<output_words;++i) {
        uint32_t actual;
        memcpy(&actual,mapped+SUBGROUP_GUARD+i*4,4);
        observed->mismatches+=actual!=subgroup_expected_word(total,i/SUBGROUP_FIELDS,i%SUBGROUP_FIELDS);
        observed->digest=(observed->digest^actual)*UINT32_C(16777619);
    }
    for (uint32_t i=0;i<SUBGROUP_GUARD;++i) {
        observed->guards+=mapped[i]!=0xcd;
        observed->guards+=mapped[SUBGROUP_GUARD+output_bytes+i]!=0xcd;
    }
    SUBGROUP_REQUIRE(!observed->mismatches && !observed->guards);
cleanup:
    /* Retain resources and the caller's device until GPU ownership is resolved. */
    if (*pending) return result;
    if (pool) vkDestroyCommandPool(device,pool,NULL);
    if (fence) vkDestroyFence(device,fence,NULL);
    if (descriptors) vkDestroyDescriptorPool(device,descriptors,NULL);
    if (mapped) vkUnmapMemory(device,memory);
    if (buffer) vkDestroyBuffer(device,buffer,NULL);
    if (memory) vkFreeMemory(device,memory,NULL);
    if (pipeline) vkDestroyPipeline(device,pipeline,NULL);
    if (module) vkDestroyShaderModule(device,module,NULL);
    if (layout) vkDestroyPipelineLayout(device,layout,NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    return result;
#undef SUBGROUP_TRY
#undef SUBGROUP_REQUIRE
}
#endif

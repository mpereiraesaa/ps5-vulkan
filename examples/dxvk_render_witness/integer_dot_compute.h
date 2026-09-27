/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public-SDK integer-dot execution. Host tests validate the instrument only. */
#ifndef PS5VK_INTEGER_DOT_COMPUTE_H
#define PS5VK_INTEGER_DOT_COMPUTE_H

enum { DOT_INVOCATIONS=128, DOT_GUARD=256, DOT_OUTPUT_BYTES=512 };
struct integer_dot_data {
    const unsigned char *inputs[3];
    uint32_t sizes[3];
    const unsigned char *expected;
};
struct integer_dot_result {
    uint32_t outputs, mismatches, guards, input_changes, digest;
    const char *step;
};

static void integer_dot_check(const struct integer_dot_data *data,
    unsigned char *const mapped[4], struct integer_dot_result *observed)
{
    observed->outputs=DOT_INVOCATIONS;
    observed->mismatches=observed->guards=observed->input_changes=0;
    observed->digest=UINT32_C(2166136261);
    for (unsigned i=0;i<DOT_INVOCATIONS;++i) {
        uint32_t actual,expected;
        memcpy(&actual,mapped[3]+DOT_GUARD+4*i,4);
        memcpy(&expected,data->expected+4*i,4);
        observed->mismatches+=actual!=expected;
        observed->digest=(observed->digest^actual)*UINT32_C(16777619);
    }
    for (unsigned b=0;b<4;++b) {
        uint32_t bytes=b==3?DOT_OUTPUT_BYTES:data->sizes[b];
        for (unsigned i=0;i<DOT_GUARD;++i) {
            observed->guards+=mapped[b][i]!=0xcd;
            observed->guards+=mapped[b][DOT_GUARD+bytes+i]!=0xcd;
        }
        if (b<3) for (unsigned i=0;i<bytes;++i)
            observed->input_changes+=mapped[b][DOT_GUARD+i]!=data->inputs[b][i];
    }
}

static VkResult integer_dot_compute_witness(VkDevice device,VkQueue queue,
    const uint32_t *words,size_t bytes,const struct integer_dot_data *data,
    VkBool32 *pending,struct integer_dot_result *observed)
{
    VkResult result=VK_SUCCESS;
    VkShaderModule module=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;
    VkBuffer buffers[4]={0};
    VkDeviceMemory memories[4]={0};
    unsigned char *mapped[4]={0};
    VkMappedMemoryRange ranges[4]={0};
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    *pending=VK_FALSE;
    memset(observed,0,sizeof(*observed));
#define DOT_TRY(call) do { observed->step=#call; result=(call); if(result!=VK_SUCCESS) goto cleanup; } while(0)
#define DOT_REQUIRE(condition) do { observed->step=#condition; if(!(condition)) { result=VK_ERROR_UNKNOWN; goto cleanup; } } while(0)
    DOT_REQUIRE(words && bytes && bytes%4==0 && data && data->expected);
    DOT_REQUIRE(data->inputs[0] && data->inputs[1] && data->inputs[2]);
    DOT_REQUIRE(data->sizes[0]==data->sizes[1] && data->sizes[2]==DOT_OUTPUT_BYTES);
    DOT_REQUIRE(data->sizes[0]==512 || data->sizes[0]==1024 || data->sizes[0]==2048);
    VkDescriptorSetLayoutBinding bindings[4]={0};
    for (unsigned i=0;i<4;++i) bindings[i]=(VkDescriptorSetLayoutBinding){
        i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=4,.pBindings=bindings};
    DOT_TRY(vkCreateDescriptorSetLayout(device,&sl,NULL,&set_layout));
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&set_layout};
    DOT_TRY(vkCreatePipelineLayout(device,&pl,NULL,&layout));
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=words};
    DOT_TRY(vkCreateShaderModule(device,&sm,NULL,&module));
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},
        .layout=layout,.basePipelineIndex=-1};
    DOT_TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    vkDestroyShaderModule(device,module,NULL); module=VK_NULL_HANDLE;
    VkDescriptorBufferInfo infos[4]={0};
    for (unsigned i=0;i<4;++i) {
        uint32_t size=i==3?DOT_OUTPUT_BYTES:data->sizes[i];
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=size+2*DOT_GUARD,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        DOT_TRY(vkCreateBuffer(device,&bi,NULL,&buffers[i]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device,buffers[i],&requirements);
        DOT_REQUIRE(requirements.memoryTypeBits&1u);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=requirements.size,.memoryTypeIndex=0};
        DOT_TRY(vkAllocateMemory(device,&ai,NULL,&memories[i]));
        DOT_TRY(vkBindBufferMemory(device,buffers[i],memories[i],0));
        DOT_TRY(vkMapMemory(device,memories[i],0,VK_WHOLE_SIZE,0,(void**)&mapped[i]));
        memset(mapped[i],0xcd,size+2*DOT_GUARD);
        if(i<3) memcpy(mapped[i]+DOT_GUARD,data->inputs[i],size);
        ranges[i]=(VkMappedMemoryRange){.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=memories[i],.size=VK_WHOLE_SIZE};
        infos[i]=(VkDescriptorBufferInfo){buffers[i],DOT_GUARD,size};
    }
    DOT_TRY(vkFlushMappedMemoryRanges(device,4,ranges));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    DOT_TRY(vkCreateDescriptorPool(device,&dpi,NULL,&descriptors));
    VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;
    DOT_TRY(vkAllocateDescriptorSets(device,&ds,&set));
    VkWriteDescriptorSet writes[4]={0};
    for (unsigned i=0;i<4;++i) writes[i]=(VkWriteDescriptorSet){
        .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&infos[i]};
    vkUpdateDescriptorSets(device,4,writes,0,NULL);
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    DOT_TRY(vkCreateCommandPool(device,&cpi,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;
    DOT_TRY(vkAllocateCommandBuffers(device,&ca,&command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    DOT_TRY(vkCreateFence(device,&fi,NULL,&fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    DOT_TRY(vkBeginCommandBuffer(command,&begin));
    VkMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,1,&before,0,NULL,0,NULL);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);
    vkCmdDispatch(command,2,1,1);
    VkMemoryBarrier after={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&after,0,NULL,0,NULL);
    DOT_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    *pending=VK_TRUE;
    DOT_TRY(vkQueueSubmit(queue,1,&submit,fence));
    DOT_TRY(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(300000000)));
    *pending=VK_FALSE;
    DOT_TRY(vkInvalidateMappedMemoryRanges(device,4,ranges));
    integer_dot_check(data,mapped,observed);
    DOT_REQUIRE(!observed->mismatches && !observed->guards && !observed->input_changes);
cleanup:
    /* Keep the caller's device and allocations alive while ownership is unresolved. */
    if (*pending) return result;
    if (pool) vkDestroyCommandPool(device,pool,NULL);
    if (fence) vkDestroyFence(device,fence,NULL);
    if (descriptors) vkDestroyDescriptorPool(device,descriptors,NULL);
    for (unsigned i=0;i<4;++i) {
        if (mapped[i]) vkUnmapMemory(device,memories[i]);
        if (buffers[i]) vkDestroyBuffer(device,buffers[i],NULL);
        if (memories[i]) vkFreeMemory(device,memories[i],NULL);
    }
    if (pipeline) vkDestroyPipeline(device,pipeline,NULL);
    if (module) vkDestroyShaderModule(device,module,NULL);
    if (layout) vkDestroyPipelineLayout(device,layout,NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    return result;
#undef DOT_TRY
#undef DOT_REQUIRE
}
#endif

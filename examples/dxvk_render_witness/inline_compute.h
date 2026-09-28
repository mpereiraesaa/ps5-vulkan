/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public Vulkan inline-uniform execution witness. Host use checks recording
 * and the oracle with a synthetic queue; it does not execute the shader. */
#ifndef PS5VK_INLINE_COMPUTE_WITNESS_H
#define PS5VK_INLINE_COMPUTE_WITNESS_H

enum { INLINE_ROUNDS = 4, INLINE_WORDS = 70, INLINE_RESULTS = 256,
       INLINE_STRIDE = 1280, INLINE_OFFSET = 256,
       INLINE_BUFFER_BYTES = INLINE_OFFSET + INLINE_ROUNDS * INLINE_STRIDE };
struct inline_compute_result { uint32_t mismatches, guards, digest; const char *step; };

/* Keep source poisoning observable even in optimized native builds. */
static void inline_witness_poison(void *data, size_t bytes)
{
    volatile unsigned char *p = data;
    for (size_t i = 0; i < bytes; ++i) p[i] = 0;
}

static VkResult inline_compute_witness_mode(VkDevice device, VkQueue queue,
    const uint32_t *words, size_t bytes, VkBool32 *pending,
    struct inline_compute_result *observed, unsigned mode)
{
    const VkBool32 boundary=mode!=0, split=mode==2;
    const unsigned set_count=split?4:1;
    const unsigned block_count=split?1:boundary?4:3, word_count=boundary?256:INLINE_WORDS;
    const unsigned payload_bytes=word_count*4;
    VkResult result = VK_SUCCESS;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout layouts[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkDescriptorUpdateTemplate update = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    unsigned char *mapped = NULL;
    *pending = VK_FALSE;
    memset(observed, 0, sizeof(*observed));
#define INLINE_TRY(call) do { observed->step = #call; result = (call); if (result != VK_SUCCESS) goto cleanup; } while (0)
#define INLINE_REQUIRE(condition) do { observed->step = #condition; if (!(condition)) { result = VK_ERROR_UNKNOWN; goto cleanup; } } while (0)
    for (unsigned i = 0; i < 2; ++i) {
        VkDescriptorSetLayoutBinding bindings[5] = {
            {0,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,i ? 4 : 20,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
            {1,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,i ? 20 : 4,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
            {2,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,256,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
            {3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
        if(boundary) {
            for(unsigned b=0;b<block_count;++b) bindings[b]=(VkDescriptorSetLayoutBinding){
                b,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,256,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
            bindings[block_count]=(VkDescriptorSetLayoutBinding){block_count,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
        }
        VkDescriptorSetLayoutCreateInfo sl = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount=i ? block_count : block_count+1,.pBindings=bindings};
        INLINE_TRY(vkCreateDescriptorSetLayout(device,&sl,NULL,&layouts[i]));
    }
    VkDescriptorSetLayout bound_layouts[4]={layouts[0],layouts[1],layouts[1],layouts[1]};
    VkPipelineLayoutCreateInfo pl = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=set_count,.pSetLayouts=bound_layouts};
    INLINE_TRY(vkCreatePipelineLayout(device,&pl,NULL,&layout));
    VkShaderModuleCreateInfo sm = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=bytes,.pCode=words};
    INLINE_TRY(vkCreateShaderModule(device,&sm,NULL,&module));
    VkComputePipelineCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},
        .layout=layout,.basePipelineIndex=-1};
    INLINE_TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    vkDestroyShaderModule(device,module,NULL); module=VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=INLINE_BUFFER_BYTES,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    INLINE_TRY(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device,buffer,&requirements);
    INLINE_REQUIRE(requirements.memoryTypeBits & 1u);
    VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=requirements.size,.memoryTypeIndex=0};
    INLINE_TRY(vkAllocateMemory(device,&ai,NULL,&memory));
    INLINE_TRY(vkBindBufferMemory(device,buffer,memory,0));
    INLINE_TRY(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,(void **)&mapped));
    memset(mapped,0xcd,INLINE_BUFFER_BYTES);
    VkMappedMemoryRange range = {.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=memory,.size=VK_WHOLE_SIZE};
    INLINE_TRY(vkFlushMappedMemoryRanges(device,1,&range));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,5*payload_bytes},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4}};
    VkDescriptorPoolInlineUniformBlockCreateInfo ip = {
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO,
        .maxInlineUniformBlockBindings=5*block_count*set_count};
    VkDescriptorPoolCreateInfo dpi = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext=&ip,.maxSets=5*set_count,.poolSizeCount=2,.pPoolSizes=sizes};
    INLINE_TRY(vkCreateDescriptorPool(device,&dpi,NULL,&descriptors));
    VkDescriptorSetLayout allocation_layouts[20];
    for(unsigned round=0;round<5;++round) for(unsigned part=0;part<set_count;++part)
        allocation_layouts[round*set_count+part]=(round==4 || part)?layouts[1]:layouts[0];
    VkDescriptorSet sets[20];
    VkDescriptorSetAllocateInfo ds = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptors,.descriptorSetCount=5*set_count,.pSetLayouts=allocation_layouts};
    INLINE_TRY(vkAllocateDescriptorSets(device,&ds,sets));
    for (unsigned round=0; round<INLINE_ROUNDS; ++round) {
        uint32_t payload[256]={0};
        for (unsigned j=0;j<word_count;++j) payload[j]=0x10203040u+round*1009u+j*37u;
        for(unsigned part=0;part<set_count;++part) {
            const unsigned update_bytes=split?256:payload_bytes;
            uint32_t *part_data=payload+(split?part*64:0);
            VkDescriptorSet destination=sets[round*set_count+part], source=sets[4*set_count+part];
            VkWriteDescriptorSetInlineUniformBlock iw = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
                .dataSize=update_bytes,.pData=part_data};
            VkWriteDescriptorSet w = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&iw,
                .dstSet=destination,.descriptorCount=update_bytes,.descriptorType=VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK};
            if (round==0) {
                const uint32_t lengths[4]={boundary?256:20,boundary?256:4,256,256};unsigned offset=0;
                for(unsigned binding=0;binding<block_count;++binding) {
                    w.dstBinding=binding;w.descriptorCount=iw.dataSize=lengths[binding];iw.pData=(unsigned char *)part_data+offset;
                    vkUpdateDescriptorSets(device,1,&w,0,NULL);offset+=lengths[binding];
                }
            } else if (round==1) {
                if(split) {
                    /* Partial writes in reverse order; rollover cannot cross sets. */
                    w.dstArrayElement=128;w.descriptorCount=iw.dataSize=128;iw.pData=part_data+32;
                    vkUpdateDescriptorSets(device,1,&w,0,NULL);
                    w.dstArrayElement=0;iw.pData=part_data;
                }
                vkUpdateDescriptorSets(device,1,&w,0,NULL); /* rollover for a single set */
            } else if (round==2) {
                w.dstSet=source;vkUpdateDescriptorSets(device,1,&w,0,NULL); /* 4/20/256 */
                VkCopyDescriptorSet copy = {.sType=VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                    .srcSet=source,.dstSet=destination,.descriptorCount=update_bytes};
                vkUpdateDescriptorSets(device,0,NULL,1,&copy);
                inline_witness_poison(part_data,update_bytes);vkUpdateDescriptorSets(device,1,&w,0,NULL);
            } else {
                unsigned char unaligned[sizeof(payload)+1];memcpy(unaligned+1,part_data,update_bytes);
                VkDescriptorUpdateTemplateEntry entry = {.descriptorCount=update_bytes,
                    .descriptorType=VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,.offset=1,.stride=SIZE_MAX};
                VkDescriptorUpdateTemplateCreateInfo ti = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
                    .descriptorUpdateEntryCount=1,.pDescriptorUpdateEntries=&entry,
                    .templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,.descriptorSetLayout=bound_layouts[part]};
                INLINE_TRY(vkCreateDescriptorUpdateTemplateKHR(device,&ti,NULL,&update));
                vkUpdateDescriptorSetWithTemplateKHR(device,destination,update,unaligned);
                inline_witness_poison(unaligned,sizeof(unaligned));
                vkDestroyDescriptorUpdateTemplateKHR(device,update,NULL);update=VK_NULL_HANDLE;
            }
        }
        inline_witness_poison(payload,sizeof(payload));
        VkDescriptorBufferInfo info = {buffer,INLINE_OFFSET+round*INLINE_STRIDE,INLINE_RESULTS*4};
        VkWriteDescriptorSet output = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[round*set_count],
            .dstBinding=block_count,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&info};
        vkUpdateDescriptorSets(device,1,&output,0,NULL);
    }
    VkCommandPoolCreateInfo cpi = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    INLINE_TRY(vkCreateCommandPool(device,&cpi,NULL,&pool));
    VkCommandBufferAllocateInfo ca = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;INLINE_TRY(vkAllocateCommandBuffers(device,&ca,&command));
    VkFenceCreateInfo fi = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    INLINE_TRY(vkCreateFence(device,&fi,NULL,&fence));
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    INLINE_TRY(vkBeginCommandBuffer(command,&begin));
    VkMemoryBarrier before = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,NULL,0,NULL);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    for(unsigned round=0;round<INLINE_ROUNDS;++round) {
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,set_count,&sets[round*set_count],0,NULL);
        vkCmdDispatch(command,INLINE_RESULTS/64,1,1);
    }
    VkMemoryBarrier after = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,NULL,0,NULL);
    INLINE_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    *pending=VK_TRUE;
    INLINE_TRY(vkQueueSubmit(queue,1,&submit,fence));
    INLINE_TRY(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(300000000)));
    *pending=VK_FALSE;
    INLINE_TRY(vkInvalidateMappedMemoryRanges(device,1,&range));
    observed->digest=2166136261u;
    for(unsigned round=0;round<INLINE_ROUNDS;++round) for(unsigned i=0;i<INLINE_RESULTS;++i) {
        uint32_t value;memcpy(&value,mapped+INLINE_OFFSET+round*INLINE_STRIDE+i*4,4);
        uint32_t expected=(0x10203040u+round*1009u+(i%word_count)*37u)*3u+(i^0x13579bdfu);
        observed->mismatches+=value!=expected;
        observed->digest=(observed->digest^value)*16777619u;
    }
    for(unsigned i=0;i<INLINE_BUFFER_BYTES;++i) {
        if(i<INLINE_OFFSET || (i-INLINE_OFFSET)%INLINE_STRIDE>=INLINE_RESULTS*4)
            observed->guards+=mapped[i]!=0xcd;
    }
    INLINE_REQUIRE(!observed->mismatches && !observed->guards);
cleanup:
    if(*pending) return result; /* Caller must retain device if ownership is unresolved. */
    if(pool) vkDestroyCommandPool(device,pool,NULL);
    if(fence) vkDestroyFence(device,fence,NULL);
    if(update) vkDestroyDescriptorUpdateTemplateKHR(device,update,NULL);
    if(descriptors) vkDestroyDescriptorPool(device,descriptors,NULL);
    if(mapped) vkUnmapMemory(device,memory);
    if(buffer) vkDestroyBuffer(device,buffer,NULL);
    if(memory) vkFreeMemory(device,memory,NULL);
    if(pipeline) vkDestroyPipeline(device,pipeline,NULL);
    if(module) vkDestroyShaderModule(device,module,NULL);
    if(layout) vkDestroyPipelineLayout(device,layout,NULL);
    for(unsigned i=0;i<2;++i) if(layouts[i]) vkDestroyDescriptorSetLayout(device,layouts[i],NULL);
    return result;
#undef INLINE_TRY
#undef INLINE_REQUIRE
}
static VkResult inline_compute_witness(VkDevice device, VkQueue queue,
    const uint32_t *words, size_t bytes, VkBool32 *pending,
    struct inline_compute_result *observed)
{
    return inline_compute_witness_mode(device,queue,words,bytes,pending,observed,VK_FALSE);
}
#endif

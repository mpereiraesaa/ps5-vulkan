/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public Vulkan image robustness instrument; host execution is not GPU evidence. */
#ifndef PS5VK_ROBUST_IMAGE_COMPUTE_H
#define PS5VK_ROBUST_IMAGE_COMPUTE_H

enum { RI_GUARD=256, RI_MAX_COORDINATES=32 };
struct robust_image_data {
    VkDescriptorType type;
    VkImageType image_type;
    VkImageViewType view_type;
    VkFormat format;
    VkExtent3D extent;
    uint32_t layers, count, image_bytes;
    VkBool32 write;
    const unsigned char *coordinates, *image, *image_after;
    const uint32_t (*expected)[4];
    const unsigned char *alpha_either;
};
struct robust_image_result {
    uint32_t outputs, mismatches, image_changes, input_changes, guards, digest;
    uint32_t values[RI_MAX_COORDINATES][4], resource_bytes, resource_digest;
    const char *step;
};

/* Mapped payloads: coordinates, shader results, upload/resource, image readback.
 * Texel buffers use payload2 directly; storage images use payload3 after copy.
 * Sampled images declare no transfer-source role; their shader cannot store. */
static void robust_image_check(const struct robust_image_data *data,
    unsigned char *const mapped[4], struct robust_image_result *out)
{
    out->outputs=data->count;out->mismatches=out->image_changes=out->input_changes=out->guards=0;
    out->digest=UINT32_C(2166136261);
    const VkBool32 texel=data->type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
        data->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    const VkBool32 sampled=data->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for(unsigned i=0;i<data->count;++i) for(unsigned c=0;c<4;++c) {
        uint32_t actual;memcpy(&actual,mapped[1]+RI_GUARD+16*i+4*c,4);
        out->values[i][c]=actual;
        VkBool32 match=actual==data->expected[i][c];
        if(c==3 && data->alpha_either[i]) match=actual<=1;
        out->mismatches+=!match;
        out->digest=(out->digest^actual)*UINT32_C(16777619);
    }
    const unsigned sizes[]={16*data->count,16*data->count,data->image_bytes,data->image_bytes};
    for(unsigned b=0;b<((texel||sampled)?3u:4u);++b) {
        for(unsigned i=0;i<RI_GUARD;++i) {
            out->guards+=mapped[b][i]!=0xcd;
            out->guards+=mapped[b][RI_GUARD+sizes[b]+i]!=0xcd;
        }
    }
    for(unsigned i=0;i<sizes[0];++i)
        out->input_changes+=mapped[0][RI_GUARD+i]!=data->coordinates[i];
    if(!texel) for(unsigned i=0;i<data->image_bytes;++i)
        out->input_changes+=mapped[2][RI_GUARD+i]!=data->image[i];
    const unsigned char *observed=sampled?NULL:mapped[texel?2:3]+RI_GUARD;
    out->resource_bytes=sampled?0:data->image_bytes;
    out->resource_digest=sampled?0:UINT32_C(2166136261);
    for(unsigned i=0;i<out->resource_bytes;++i) {
        out->image_changes+=observed[i]!=data->image_after[i];
        out->resource_digest=(out->resource_digest^observed[i])*UINT32_C(16777619);
    }
}

static VkResult robust_image_compute(VkDevice device,VkQueue queue,
    const uint32_t *words,size_t bytes,const struct robust_image_data *data,
    VkBool32 *pending,struct robust_image_result *out)
{
    VkResult result=VK_SUCCESS;
    VkBuffer buffers[4]={0};VkDeviceMemory memories[4]={0};unsigned char *mapped[4]={0};
    VkImage image=VK_NULL_HANDLE;VkDeviceMemory image_memory=VK_NULL_HANDLE;
    VkImageView image_view=VK_NULL_HANDLE;VkBufferView buffer_view=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;VkShaderModule module=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;VkDescriptorPool descriptors=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    VkMappedMemoryRange ranges[4]={0};
    *pending=VK_FALSE;memset(out,0,sizeof(*out));
#define RI_TRY(call) do {out->step=#call;result=(call);if(result!=VK_SUCCESS)goto cleanup;}while(0)
#define RI_REQUIRE(ok) do {out->step=#ok;if(!(ok)){result=VK_ERROR_UNKNOWN;goto cleanup;}}while(0)
    RI_REQUIRE(data && words && bytes && bytes%4==0 && data->count && data->count<=RI_MAX_COORDINATES);
    RI_REQUIRE(data->coordinates && data->image && data->image_after && data->expected && data->alpha_either);
    RI_REQUIRE(data->image_bytes && data->image_bytes<=UINT32_MAX-2*RI_GUARD);
    const VkBool32 texel=data->type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
        data->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    RI_REQUIRE(texel || data->type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
        data->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    const VkBool32 sampled=data->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const unsigned buffer_count=(texel||sampled)?3:4;
    const unsigned sizes[]={16*data->count,16*data->count,data->image_bytes,data->image_bytes};
    for(unsigned b=0;b<buffer_count;++b) {
        VkBufferUsageFlags usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if(b==2) usage=texel?(data->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER?
            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT:VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT):VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if(b==3) usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizes[b]+2*RI_GUARD,.usage=usage};
        RI_TRY(vkCreateBuffer(device,&bi,NULL,&buffers[b]));
        VkMemoryRequirements mr;vkGetBufferMemoryRequirements(device,buffers[b],&mr);
        RI_REQUIRE(mr.memoryTypeBits&1u);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=0};
        RI_TRY(vkAllocateMemory(device,&ai,NULL,&memories[b]));
        RI_TRY(vkBindBufferMemory(device,buffers[b],memories[b],0));
        RI_TRY(vkMapMemory(device,memories[b],0,VK_WHOLE_SIZE,0,(void**)&mapped[b]));
        memset(mapped[b],0xcd,sizes[b]+2*RI_GUARD);
        if(b==0)memcpy(mapped[b]+RI_GUARD,data->coordinates,sizes[b]);
        if(b==2)memcpy(mapped[b]+RI_GUARD,data->image,sizes[b]);
        ranges[b]=(VkMappedMemoryRange){.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memories[b],.size=VK_WHOLE_SIZE};
    }
    RI_TRY(vkFlushMappedMemoryRanges(device,buffer_count,ranges));
    VkImageSubresourceRange sub={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,data->layers};
    if(texel) {
        VkBufferViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .buffer=buffers[2],.format=data->format,.offset=RI_GUARD,.range=data->image_bytes};
        RI_TRY(vkCreateBufferView(device,&vi,NULL,&buffer_view));
    } else {
        VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=data->image_type,
            .format=data->format,.extent=data->extent,.mipLevels=1,.arrayLayers=data->layers,
            .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=(sampled?0:VK_IMAGE_USAGE_TRANSFER_SRC_BIT)|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
                (data->type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?VK_IMAGE_USAGE_STORAGE_BIT:VK_IMAGE_USAGE_SAMPLED_BIT)};
        RI_TRY(vkCreateImage(device,&ii,NULL,&image));
        VkMemoryRequirements mr;vkGetImageMemoryRequirements(device,image,&mr);RI_REQUIRE(mr.memoryTypeBits&1u);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=0};
        RI_TRY(vkAllocateMemory(device,&ai,NULL,&image_memory));RI_TRY(vkBindImageMemory(device,image,image_memory,0));
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
            .viewType=data->view_type,.format=data->format,.subresourceRange=sub};
        RI_TRY(vkCreateImageView(device,&vi,NULL,&image_view));
        if(data->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,.borderColor=VK_BORDER_COLOR_INT_TRANSPARENT_BLACK};
            RI_TRY(vkCreateSampler(device,&si,NULL,&sampler));
        }
    }
    VkDescriptorSetLayoutBinding bindings[3]={{0,data->type,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=3,.pBindings=bindings};
    RI_TRY(vkCreateDescriptorSetLayout(device,&sl,NULL,&set_layout));
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};
    RI_TRY(vkCreatePipelineLayout(device,&pl,NULL,&layout));
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=words};
    RI_TRY(vkCreateShaderModule(device,&sm,NULL,&module));
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=layout,.basePipelineIndex=-1};
    RI_TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    VkDescriptorPoolSize ps[2]={{data->type,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
    RI_TRY(vkCreateDescriptorPool(device,&pi,NULL,&descriptors));
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&set_layout};VkDescriptorSet set;
    RI_TRY(vkAllocateDescriptorSets(device,&da,&set));
    VkDescriptorImageInfo im={sampler,image_view,sampled?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo infos[2]={{buffers[0],RI_GUARD,sizes[0]},{buffers[1],RI_GUARD,sizes[1]}};
    VkWriteDescriptorSet writes[3]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.descriptorCount=1,
        .descriptorType=data->type,.pImageInfo=texel?NULL:&im,.pTexelBufferView=texel?&buffer_view:NULL}};
    for(unsigned b=1;b<3;++b)writes[b]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=b,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&infos[b-1]};
    vkUpdateDescriptorSets(device,3,writes,0,NULL);
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};RI_TRY(vkCreateCommandPool(device,&cp,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;
    RI_TRY(vkAllocateCommandBuffers(device,&ca,&command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};RI_TRY(vkCreateFence(device,&fi,NULL,&fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};RI_TRY(vkBeginCommandBuffer(command,&begin));
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&host,0,NULL,0,NULL);
    VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=sampled?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=sub};
    VkBufferImageCopy copy={.bufferOffset=RI_GUARD,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,data->layers},.imageExtent=data->extent};
    if(!texel) {
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ib);
        vkCmdCopyBufferToImage(command,buffers[2],image,ib.newLayout,1,&copy);
        if(sampled) {
            ib.oldLayout=ib.newLayout;ib.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ib.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&ib);
        }
        VkMemoryBarrier uploaded={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&uploaded,0,NULL,0,NULL);
    }
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);
    vkCmdDispatch(command,1,1,1);
    if(!texel && !sampled) {
        VkMemoryBarrier computed={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&computed,0,NULL,0,NULL);
        vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_GENERAL,buffers[3],1,&copy);
    }
    VkMemoryBarrier after={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,NULL,0,NULL);
    RI_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    *pending=VK_TRUE;RI_TRY(vkQueueSubmit(queue,1,&submit,fence));
    RI_TRY(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(300000000)));*pending=VK_FALSE;
    RI_TRY(vkInvalidateMappedMemoryRanges(device,buffer_count,ranges));
    robust_image_check(data,mapped,out);
    RI_REQUIRE(!out->mismatches && !out->image_changes && !out->input_changes && !out->guards);
cleanup:
    if(*pending)return result;
    if(pool)vkDestroyCommandPool(device,pool,NULL);
    if(fence)vkDestroyFence(device,fence,NULL);
    if(descriptors)vkDestroyDescriptorPool(device,descriptors,NULL);
    if(buffer_view)vkDestroyBufferView(device,buffer_view,NULL);
    if(image_view)vkDestroyImageView(device,image_view,NULL);
    if(sampler)vkDestroySampler(device,sampler,NULL);
    if(image)vkDestroyImage(device,image,NULL);
    if(image_memory)vkFreeMemory(device,image_memory,NULL);
    for(unsigned b=0;b<4;++b) {
        if(mapped[b])vkUnmapMemory(device,memories[b]);
        if(buffers[b])vkDestroyBuffer(device,buffers[b],NULL);
        if(memories[b])vkFreeMemory(device,memories[b],NULL);
    }
    if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
    if(module)vkDestroyShaderModule(device,module,NULL);
    if(layout)vkDestroyPipelineLayout(device,layout,NULL);
    if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    return result;
#undef RI_TRY
#undef RI_REQUIRE
}
#endif

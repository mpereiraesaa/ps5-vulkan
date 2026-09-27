/* SPDX-License-Identifier: GPL-3.0-or-later
 * SDK-only bit-exact integer-dot graphics execution. No presentation path. */
#ifndef PS5VK_INTEGER_DOT_GRAPHICS_H
#define PS5VK_INTEGER_DOT_GRAPHICS_H

enum { DOT_GFX_WIDTH=16,DOT_GFX_HEIGHT=8,DOT_GFX_INPUT=32768,DOT_GFX_IMAGE=512,DOT_GFX_GUARD=256 };
struct integer_dot_graphics_case {
    /* VS,TCS,TES,GS,FS; null stages omitted, TCS/TES must be paired. */
    const uint32_t *words[5];
    size_t bytes[5];
    const unsigned char *records,*expected_rgba;
};
struct integer_dot_graphics_result {
    uint32_t pixels,mismatches,guards,input_changes,digest;
    const char *step;
};
struct dot_graphics_buffer {
    VkBuffer buffer;VkDeviceMemory memory;unsigned char *mapped;uint32_t size;
};
static void integer_dot_graphics_check(const struct integer_dot_graphics_case *data,
    const unsigned char *input,const unsigned char *image,struct integer_dot_graphics_result *out)
{
    out->pixels=128;out->mismatches=out->guards=out->input_changes=0;out->digest=UINT32_C(2166136261);
    for(unsigned i=0;i<128;++i) {
        const unsigned char *p=image+DOT_GFX_GUARD+4*i;
        out->mismatches+=memcmp(p,data->expected_rgba+4*i,4)!=0;
        uint32_t word=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        out->digest=(out->digest^word)*UINT32_C(16777619);
    }
    for(unsigned i=0;i<DOT_GFX_INPUT;++i)out->input_changes+=input[DOT_GFX_GUARD+i]!=data->records[i];
    for(unsigned i=0;i<DOT_GFX_GUARD;++i) {
        out->guards+=input[i]!=0xcd;out->guards+=input[DOT_GFX_GUARD+DOT_GFX_INPUT+i]!=0xcd;
        out->guards+=image[i]!=0xcd;out->guards+=image[DOT_GFX_GUARD+DOT_GFX_IMAGE+i]!=0xcd;
    }
}
static VkResult dot_graphics_buffer_create(VkDevice device,uint32_t size,VkBufferUsageFlags usage,
    const unsigned char *data,struct dot_graphics_buffer *out)
{
    out->size=size;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size+2*DOT_GFX_GUARD,.usage=usage};
    VkResult r=vkCreateBuffer(device,&bi,NULL,&out->buffer);if(r!=VK_SUCCESS)return r;
    VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,out->buffer,&requirements);
    if(!(requirements.memoryTypeBits&1u))return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=0};
    r=vkAllocateMemory(device,&ai,NULL,&out->memory);if(r!=VK_SUCCESS)return r;
    r=vkBindBufferMemory(device,out->buffer,out->memory,0);if(r!=VK_SUCCESS)return r;
    r=vkMapMemory(device,out->memory,0,VK_WHOLE_SIZE,0,(void**)&out->mapped);if(r!=VK_SUCCESS)return r;
    memset(out->mapped,0xcd,size+2*DOT_GFX_GUARD);
    if(data)memcpy(out->mapped+DOT_GFX_GUARD,data,size);
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=out->memory,.size=VK_WHOLE_SIZE};
    return vkFlushMappedMemoryRanges(device,1,&range);
}
static void dot_graphics_buffer_destroy(VkDevice device,struct dot_graphics_buffer *b)
{
    if(b->mapped)vkUnmapMemory(device,b->memory);
    if(b->buffer)vkDestroyBuffer(device,b->buffer,NULL);
    if(b->memory)vkFreeMemory(device,b->memory,NULL);
}
static VkResult integer_dot_graphics_witness(VkDevice device,VkQueue queue,
    const struct integer_dot_graphics_case *data,VkBool32 *pending,struct integer_dot_graphics_result *out)
{
    VkResult result=VK_SUCCESS;
    struct dot_graphics_buffer input={0},staging={0};
    VkImage image=VK_NULL_HANDLE;VkDeviceMemory image_memory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;
    VkRenderPass render_pass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;VkDescriptorPool descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;VkShaderModule modules[5]={0};
    VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    *pending=VK_FALSE;memset(out,0,sizeof(*out));
#define GFX_TRY(call) do { out->step=#call;result=(call);if(result!=VK_SUCCESS)goto cleanup; } while(0)
#define GFX_REQUIRE(ok) do { out->step=#ok;if(!(ok)){result=VK_ERROR_UNKNOWN;goto cleanup;} } while(0)
    GFX_REQUIRE(data && data->records && data->expected_rgba && data->words[0] && data->words[4]);
    GFX_REQUIRE(!!data->words[1]==!!data->words[2]);
    for(unsigned i=0;i<5;++i)GFX_REQUIRE(data->words[i]?(data->bytes[i]>=20 && data->bytes[i]%4==0):data->bytes[i]==0);
    GFX_TRY(dot_graphics_buffer_create(device,DOT_GFX_INPUT,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,data->records,&input));
    GFX_TRY(dot_graphics_buffer_create(device,DOT_GFX_IMAGE,VK_BUFFER_USAGE_TRANSFER_DST_BIT,NULL,&staging));
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={DOT_GFX_WIDTH,DOT_GFX_HEIGHT,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    GFX_TRY(vkCreateImage(device,&ii,NULL,&image));
    VkMemoryRequirements requirements;vkGetImageMemoryRequirements(device,image,&requirements);
    GFX_REQUIRE(requirements.memoryTypeBits&1u);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=0};
    GFX_TRY(vkAllocateMemory(device,&ai,NULL,&image_memory));GFX_TRY(vkBindImageMemory(device,image,image_memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    GFX_TRY(vkCreateImageView(device,&vi,NULL,&view));
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
        .subpassCount=1,.pSubpasses=&subpass};
    GFX_TRY(vkCreateRenderPass(device,&rp,NULL,&render_pass));
    VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=render_pass,
        .attachmentCount=1,.pAttachments=&view,.width=DOT_GFX_WIDTH,.height=DOT_GFX_HEIGHT,.layers=1};
    GFX_TRY(vkCreateFramebuffer(device,&fb,NULL,&framebuffer));
    VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_ALL_GRAPHICS,NULL};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    GFX_TRY(vkCreateDescriptorSetLayout(device,&sl,NULL,&set_layout));
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};
    GFX_TRY(vkCreatePipelineLayout(device,&pl,NULL,&layout));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    GFX_TRY(vkCreateDescriptorPool(device,&dp,NULL,&descriptors));
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptors,
        .descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;GFX_TRY(vkAllocateDescriptorSets(device,&da,&set));
    VkDescriptorBufferInfo buffer_info={input.buffer,DOT_GFX_GUARD,DOT_GFX_INPUT};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_info};
    vkUpdateDescriptorSets(device,1,&write,0,NULL);
    const VkShaderStageFlagBits stage_bits[5]={VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,VK_SHADER_STAGE_GEOMETRY_BIT,VK_SHADER_STAGE_FRAGMENT_BIT};
    VkPipelineShaderStageCreateInfo stages[5]={0};uint32_t stage_count=0;
    for(unsigned i=0;i<5;++i)if(data->words[i]) {
        VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=data->bytes[i],.pCode=data->words[i]};
        GFX_TRY(vkCreateShaderModule(device,&sm,NULL,&modules[i]));
        stages[stage_count++]=(VkPipelineShaderStageCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage=stage_bits[i],.module=modules[i],.pName="main"};
    }
    VkPipelineVertexInputStateCreateInfo vertices={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=data->words[1]?VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineTessellationStateCreateInfo tessellation={.sType=VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,.patchControlPoints=3};
    VkViewport viewport={0,0,DOT_GFX_WIDTH,DOT_GFX_HEIGHT,0,1};VkRect2D scissor={{0,0},{DOT_GFX_WIDTH,DOT_GFX_HEIGHT}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend_attachment};
    VkGraphicsPipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=stage_count,.pStages=stages,
        .pVertexInputState=&vertices,.pInputAssemblyState=&assembly,.pTessellationState=data->words[1]?&tessellation:NULL,
        .pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pColorBlendState=&blend,
        .layout=layout,.renderPass=render_pass,.basePipelineIndex=-1};
    GFX_TRY(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};GFX_TRY(vkCreateCommandPool(device,&cp,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;GFX_TRY(vkAllocateCommandBuffers(device,&ca,&command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};GFX_TRY(vkCreateFence(device,&fi,NULL,&fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    GFX_TRY(vkBeginCommandBuffer(command,&begin));
    /* Input writes were flushed before submission. Queue submission provides
     * their host-to-device dependency; no explicit host/shader barrier needed. */
    VkImageMemoryBarrier to_color={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&to_color);
    VkClearValue clear={.color={{0.25f,0.5f,0.75f,1.0f}}};
    VkRenderPassBeginInfo rb={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=render_pass,.framebuffer=framebuffer,
        .renderArea={{0,0},{DOT_GFX_WIDTH,DOT_GFX_HEIGHT}},.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(command,&rb,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
    vkCmdDraw(command,6,128,0,0);vkCmdEndRenderPass(command);
    VkImageMemoryBarrier to_transfer=to_color;to_transfer.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_transfer.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;to_transfer.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_transfer.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&to_transfer);
    VkBufferImageCopy copy={.bufferOffset=DOT_GFX_GUARD,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={DOT_GFX_WIDTH,DOT_GFX_HEIGHT,1}};
    vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging.buffer,1,&copy);
    VkMemoryBarrier publication={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&publication,0,NULL,0,NULL);
    GFX_TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    *pending=VK_TRUE;GFX_TRY(vkQueueSubmit(queue,1,&submit,fence));
    GFX_TRY(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(300000000)));*pending=VK_FALSE;
    VkMappedMemoryRange ranges[2]={{.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=input.memory,.size=VK_WHOLE_SIZE},
        {.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=staging.memory,.size=VK_WHOLE_SIZE}};
    GFX_TRY(vkInvalidateMappedMemoryRanges(device,2,ranges));
    integer_dot_graphics_check(data,input.mapped,staging.mapped,out);
    GFX_REQUIRE(!out->mismatches && !out->guards && !out->input_changes);
cleanup:
    if(*pending)return result;
    if(pool)vkDestroyCommandPool(device,pool,NULL);
    if(fence)vkDestroyFence(device,fence,NULL);
    if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
    for(unsigned i=0;i<5;++i)if(modules[i])vkDestroyShaderModule(device,modules[i],NULL);
    if(framebuffer)vkDestroyFramebuffer(device,framebuffer,NULL);
    if(render_pass)vkDestroyRenderPass(device,render_pass,NULL);
    if(view)vkDestroyImageView(device,view,NULL);
    if(image)vkDestroyImage(device,image,NULL);
    if(image_memory)vkFreeMemory(device,image_memory,NULL);
    if(descriptors)vkDestroyDescriptorPool(device,descriptors,NULL);
    if(layout)vkDestroyPipelineLayout(device,layout,NULL);
    if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    dot_graphics_buffer_destroy(device,&input);dot_graphics_buffer_destroy(device,&staging);
    return result;
#undef GFX_TRY
#undef GFX_REQUIRE
}
#endif

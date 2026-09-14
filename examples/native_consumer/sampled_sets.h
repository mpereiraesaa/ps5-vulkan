/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public SDK consumer: no native/backend headers or private symbols.
 * Included after the consumer's CHECK/REQUIRE and owned shader declarations. */
#include "sampled_set_shaders.h"

static unsigned sampled_source(unsigned set,unsigned element,unsigned round)
{
    uint32_t x=(1+24*set+element)*UINT32_C(0x9e3779b9)+round*UINT32_C(0x7f4a7c15);
    x^=x>>16;x*=UINT32_C(0x85ebca6b);
    return x>>30;
}

static uint32_t sampled_expected(unsigned round)
{
    unsigned sums[4]={0};
    for(unsigned s=0;s<4;++s)for(unsigned e=0;e<24;++e) {
        unsigned source=sampled_source(s,e,round),weight=1+24*s+e;
        for(unsigned component=0;component<4;++component)
            if(component==3 || source==3 || source==component)sums[component]+=weight;
    }
    /* BGRA8 readback; inputs are exactly 0/1 and weights are small integers. */
    unsigned r=(sums[0]*255+4096)/8192,g=(sums[1]*255+4096)/8192;
    unsigned b=(sums[2]*255+4096)/8192,a=(sums[3]*255+4096)/8192;
    return b|(g<<8)|(r<<16)|(a<<24);
}

static void run_sampled_sets(VkDevice device,VkQueue queue)
{
    ps5log_line(PS5LOG_MARK,"PS5VK_CONSUMER_SAMPLED_SETS_START sets=4 descriptors=96 rounds=4");
    VkImage textures[4],target;VkImageView views[4],target_view;
    VkDeviceMemory texture_memory[4],target_memory,staging_memory;
    VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={4,4,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=image_info.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    for(unsigned i=0;i<5;++i) {
        VkImage *image=i<4?textures+i:&target;
        VkDeviceMemory *memory=i<4?texture_memory+i:&target_memory;
        if(i==4) {
            image_info.format=view_info.format=VK_FORMAT_B8G8R8A8_UNORM;
            image_info.extent=(VkExtent3D){1920,1080,1};
            image_info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        CHECK(vkCreateImage(device,&image_info,NULL,image));
        VkMemoryRequirements requirements;vkGetImageMemoryRequirements(device,*image,&requirements);
        VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=requirements.size,.memoryTypeIndex=0};
        CHECK(vkAllocateMemory(device,&allocation,NULL,memory));
        CHECK(vkBindImageMemory(device,*image,*memory,0));
        view_info.image=*image;
        CHECK(vkCreateImageView(device,&view_info,NULL,i<4?views+i:&target_view));
    }
    VkBuffer staging;
    VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=256,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    CHECK(vkCreateBuffer(device,&buffer_info,NULL,&staging));
    VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,staging,&requirements);
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=requirements.size,.memoryTypeIndex=0};
    CHECK(vkAllocateMemory(device,&allocation,NULL,&staging_memory));
    CHECK(vkBindBufferMemory(device,staging,staging_memory,0));
    uint32_t *upload=NULL;CHECK(vkMapMemory(device,staging_memory,0,VK_WHOLE_SIZE,0,(void **)&upload));
    const uint32_t rgba[4]={0xff0000ff,0xff00ff00,0xffff0000,0xffffffff};
    for(unsigned i=0;i<4;++i)for(unsigned p=0;p<16;++p)upload[16*i+p]=rgba[i];
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=staging_memory,.size=VK_WHOLE_SIZE};
    CHECK(vkFlushMappedMemoryRanges(device,1,&range));vkUnmapMemory(device,staging_memory);
    VkSampler sampler;
    VkSamplerCreateInfo sampler_info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(vkCreateSampler(device,&sampler_info,NULL,&sampler));
    VkDescriptorSetLayout set_layout,layouts[4];
    VkDescriptorSetLayoutBinding binding={.binding=7,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount=24,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    CHECK(vkCreateDescriptorSetLayout(device,&set_info,NULL,&set_layout));
    for(unsigned s=0;s<4;++s)layouts[s]=set_layout;
    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=4,.pSetLayouts=layouts};
    CHECK(vkCreatePipelineLayout(device,&layout_info,NULL,&layout));
    VkDescriptorPool descriptor_pool;VkDescriptorSet sets[4];
    VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,96};
    VkDescriptorPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=4,.poolSizeCount=1,.pPoolSizes=&pool_size};
    CHECK(vkCreateDescriptorPool(device,&pool_info,NULL,&descriptor_pool));
    VkDescriptorSetAllocateInfo set_allocation={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptor_pool,.descriptorSetCount=4,.pSetLayouts=layouts};
    CHECK(vkAllocateDescriptorSets(device,&set_allocation,sets));
    VkAttachmentDescription attachment={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&ref};
    VkRenderPassCreateInfo pass_info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass pass;CHECK(vkCreateRenderPass(device,&pass_info,NULL,&pass));
    VkFramebufferCreateInfo framebuffer_info={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=pass,.attachmentCount=1,.pAttachments=&target_view,.width=1920,.height=1080,.layers=1};
    VkFramebuffer framebuffer;CHECK(vkCreateFramebuffer(device,&framebuffer_info,NULL,&framebuffer));
    VkShaderModule modules[2];
    VkShaderModuleCreateInfo module={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(consumer_vertex_spirv),.pCode=consumer_vertex_spirv};
    CHECK(vkCreateShaderModule(device,&module,NULL,modules));
    module.codeSize=sizeof(consumer_sampled_sets_spirv);module.pCode=consumer_sampled_sets_spirv;
    CHECK(vkCreateShaderModule(device,&module,NULL,modules+1));
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,1920,1080,0,1};VkRect2D scissor={{0,0},{1920,1080}};
    VkPipelineViewportStateCreateInfo viewport_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo multisample={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline_info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vertex,.pInputAssemblyState=&assembly,
        .pViewportState=&viewport_info,.pRasterizationState=&raster,.pMultisampleState=&multisample,
        .pColorBlendState=&blend,.layout=layout,.renderPass=pass};
    VkPipeline pipeline;CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipeline_info,NULL,&pipeline));
    VkCommandPool command_pool;
    VkCommandPoolCreateInfo command_pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    CHECK(vkCreateCommandPool(device,&command_pool_info,NULL,&command_pool));
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(vkAllocateCommandBuffers(device,&command_info,&command));
    uint32_t *pixels=NULL;CHECK(vkMapMemory(device,target_memory,0,VK_WHOLE_SIZE,0,(void **)&pixels));
    for(unsigned round=0;round<4;++round) {
        CHECK(vkResetCommandBuffer(command,0));
        for(unsigned s=0;s<4;++s) {
            VkDescriptorImageInfo images[24];
            for(unsigned e=0;e<24;++e)images[e]=(VkDescriptorImageInfo){sampler,
                views[sampled_source(s,e,round)],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[s],
                .dstBinding=7,.descriptorCount=24,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=images};
            vkUpdateDescriptorSets(device,1,&write,0,NULL);
        }
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(command,&begin));
        if(!round)for(unsigned i=0;i<4;++i) {
            VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=textures[i],
                .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,0,NULL,0,NULL,1,&barrier);
            VkBufferImageCopy copy={.bufferOffset=64*i,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                .imageExtent={4,4,1}};
            vkCmdCopyBufferToImage(command,staging,textures[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0,0,NULL,0,NULL,1,&barrier);
        }
        VkClearValue clear={.color={.float32={0,0,0,1}}};
        VkRenderPassBeginInfo begin_pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
            .framebuffer=framebuffer,.renderArea={{0,0},{1920,1080}},.clearValueCount=1,.pClearValues=&clear};
        vkCmdBeginRenderPass(command,&begin_pass,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        for(unsigned s=4;s--;)vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,s,1,sets+s,0,NULL);
        vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
        CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        range.memory=target_memory;CHECK(vkInvalidateMappedMemoryRanges(device,1,&range));
        uint32_t expected=sampled_expected(round);unsigned changed=0,bad=0;
        for(unsigned p=0;p<1920*1080;++p) {
            if(pixels[p]==0xff000000)continue;
            ++changed;bad+=pixels[p]!=expected;
        }
        ps5log_printf(PS5LOG_MARK,"PS5VK_CONSUMER_SAMPLED_SETS_RESULT round=%u expected=%08x changed=%u bad=%u",
            round,expected,changed,bad);
        REQUIRE(changed==471744 && !bad,"96 sampled descriptors: exact weighted GPU readback");
    }
    vkUnmapMemory(device,target_memory);vkDestroyCommandPool(device,command_pool,NULL);
    vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);
    for(unsigned i=0;i<2;++i)vkDestroyShaderModule(device,modules[i],NULL);
    vkDestroyDescriptorPool(device,descriptor_pool,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    vkDestroySampler(device,sampler,NULL);vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyRenderPass(device,pass,NULL);
    vkDestroyImageView(device,target_view,NULL);vkDestroyImage(device,target,NULL);vkFreeMemory(device,target_memory,NULL);
    for(unsigned i=0;i<4;++i) {
        vkDestroyImageView(device,views[i],NULL);vkDestroyImage(device,textures[i],NULL);vkFreeMemory(device,texture_memory[i],NULL);
    }
    vkDestroyBuffer(device,staging,NULL);vkFreeMemory(device,staging_memory,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_CONSUMER_SAMPLED_SETS_RETIRED");
}

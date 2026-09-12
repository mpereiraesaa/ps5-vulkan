#include "texture_descriptor.h"
#include "vk_descriptor.h"
#include "vk_command.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static VkResult allocate(void *c,VkDeviceSize n,void **a,void **b)
{(void)c;*a=aligned_alloc(256,(n+255)&~(VkDeviceSize)255);*b=*a;return *a?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;}
static void release(void *c,void *p){(void)c;free(p);}
static VkResult cache(void *c,void *p,VkDeviceSize o,VkDeviceSize n)
{(void)c;(void)p;(void)o;(void)n;return VK_SUCCESS;}
int main(void)
{
    struct VkDevice_T d={.graphics_enabled=VK_TRUE,.image_requirements=ps5vk_native_image_requirements,
        .memory={.allocate=allocate,.release=release,.flush=cache,.invalidate=cache},.max_allocation=4096,.noncoherent_atom=64,.buffer_alignment=256};
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={65,3,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImage image;assert(vkCreateImage(&d,&ii,NULL,&image)==VK_SUCCESS);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=2048};
    VkDeviceMemory memory;assert(vkAllocateMemory(&d,&ai,NULL,&memory)==VK_SUCCESS);
    assert(vkBindImageMemory(&d,image,memory,256)==VK_SUCCESS);
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=ii.format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view;assert(vkCreateImageView(&d,&vi,NULL,&view)==VK_SUCCESS);
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR};
    VkSampler sampler;assert(vkCreateSampler(&d,&si,NULL,&sampler)==VK_SUCCESS);
    uint32_t words[12];assert(ps5vk_texture_descriptor(&d,view,sampler,words)==VK_SUCCESS);
    void *base;VkDeviceSize bytes;assert(ps5vk_image_span(&d,image,&base,&bytes)==VK_SUCCESS);
    assert(words[0]==(uint32_t)((uintptr_t)base>>8));
    assert(words[1]==((uint32_t)((uintptr_t)base>>40)|(56u<<20)));
    assert(words[2]==(16u|(2u<<14)|(1u<<31)) && words[3]==0x90000fac);
    assert(words[4]==127 && words[5]==0x400000 && !words[6] && !words[7]);
    assert(!memcmp(words+8,sampler->words,16));
    /* All accepted axis/filter combinations must survive API creation and
     * descriptor assembly independently. Encoding is not sampling accuracy. */
    for(unsigned axes=0;axes<8;++axes)for(unsigned filters=0;filters<4;++filters)
        for(unsigned mip=0;mip<2;++mip) {
            VkSamplerCreateInfo variant={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter=(filters&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
                .minFilter=(filters&2)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
                .mipmapMode=mip?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU=(axes&1)?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT,
                .addressModeV=(axes&2)?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT,
                .addressModeW=(axes&4)?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT};
            VkSampler candidate;
            assert(vkCreateSampler(&d,&variant,NULL,&candidate)==VK_SUCCESS);
            uint32_t descriptor[12];
            assert(ps5vk_texture_descriptor(&d,view,candidate,descriptor)==VK_SUCCESS);
            assert(!memcmp(descriptor,words,8*sizeof(uint32_t)));
            assert(descriptor[8]==((axes&1?2u:0u)|(axes&2?16u:0u)|(axes&4?128u:0u)));
            assert(!descriptor[9] && !descriptor[11]);
            assert(descriptor[10]==((filters&1?1u<<20:0u)|(filters&2?1u<<22:0u)));
            vkDestroySampler(&d,candidate,NULL);
            assert(d.sampler_objects==1); /* Original fixture stays alive. */
        }
    uint32_t saved[12];memcpy(saved,words,sizeof(words));
    image->info.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    assert(ps5vk_texture_descriptor(&d,view,sampler,words)!=VK_SUCCESS && !memcmp(saved,words,sizeof(words)));
    image->info.usage=VK_IMAGE_USAGE_SAMPLED_BIT;view->range.baseMipLevel=1;
    assert(ps5vk_texture_descriptor(&d,view,sampler,words)!=VK_SUCCESS);
    view->range.baseMipLevel=0;image->requirements.size=256;
    assert(ps5vk_texture_descriptor(&d,view,sampler,words)!=VK_SUCCESS);
    image->requirements.size=1536;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=1024,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer buffer;assert(vkCreateBuffer(&d,&bi,NULL,&buffer)==VK_SUCCESS);
    VkDeviceMemory upload;assert(vkAllocateMemory(&d,&ai,NULL,&upload)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,buffer,upload,256)==VK_SUCCESS);
    VkCommandPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool commands;assert(vkCreateCommandPool(&d,&ci,NULL,&commands)==VK_SUCCESS);
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.commandBufferCount=1};
    VkCommandBuffer cb;assert(vkAllocateCommandBuffers(&d,&ca,&cb)==VK_SUCCESS);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(cb,&begin)==VK_SUCCESS);
    image->info.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkBufferImageCopy region={.bufferOffset=16,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={65,3,1}};
    vkCmdCopyBufferToImage(cb,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    assert(cb->state==PS5VK_RECORDING && cb->operation_count==1 && cb->operations[0].copy_region.bufferOffset==16);
    region.bufferOffset=32;assert(cb->operations[0].copy_region.bufferOffset==16);
    assert(vkEndCommandBuffer(cb)==VK_SUCCESS);cb->state=PS5VK_PENDING;
    assert(!d.invalidate(&d,VK_OBJECT_TYPE_BUFFER,buffer) && !d.invalidate(&d,VK_OBJECT_TYPE_IMAGE,image));
    cb->state=PS5VK_EXECUTABLE;
    assert(vkBeginCommandBuffer(cb,&begin)==VK_SUCCESS);
    VkBufferImageCopy regions[2]={region,region};regions[1].imageExtent.width=66;
    vkCmdCopyBufferToImage(cb,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,2,regions);
    assert(cb->state==PS5VK_INVALID && !cb->operation_count);
    vkDestroyCommandPool(&d,commands,NULL);vkDestroyBuffer(&d,buffer,NULL);vkFreeMemory(&d,upload,NULL);
    VkDescriptorSetLayoutBinding binding={.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount=2,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    VkDescriptorSetLayout set_layout;assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&set_layout)==VK_SUCCESS);
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,
        .poolSizeCount=1,.pPoolSizes=&size};
    VkDescriptorPool pool;assert(vkCreateDescriptorPool(&d,&pi,NULL,&pool)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo sa={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;assert(vkAllocateDescriptorSets(&d,&sa,&set)==VK_SUCCESS);
    VkDescriptorImageInfo infos[2]={{sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler,view,VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,
        .descriptorCount=2,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=infos};
    vkUpdateDescriptorSets(&d,1,&write,0,NULL);assert(set->generation==1 && !set->defined[0]);
    infos[1].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
    vkUpdateDescriptorSets(&d,1,&write,0,NULL);
    assert(set->generation==2 && set->defined[0] && set->defined[1] && set->image_resources[0]==image);
    assert(set->images[0].sampler==sampler && set->images[1].imageView==view);
    infos[0].sampler=NULL;assert(set->images[0].sampler==sampler);
    set->pending=1;vkUpdateDescriptorSets(&d,1,&write,0,NULL);assert(set->generation==2);
    set->pending=0;
    VkCopyDescriptorSet copy={.sType=VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,.srcSet=set,.dstSet=set,
        .srcArrayElement=1,.descriptorCount=1};
    vkUpdateDescriptorSets(&d,0,NULL,1,&copy);
    assert(set->generation==3 && set->images[0].imageLayout==VK_IMAGE_LAYOUT_GENERAL && set->image_resources[0]==image);
    vkDestroyDescriptorPool(&d,pool,NULL);vkDestroyDescriptorSetLayout(&d,set_layout,NULL);
    vkDestroySampler(&d,sampler,NULL);vkDestroyImageView(&d,view,NULL);
    vkDestroyImage(&d,image,NULL);vkFreeMemory(&d,memory,NULL);assert(!d.graphics_objects);
}

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
    static const VkSamplerAddressMode modes[4]={VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    static const unsigned native_modes[4]={0,1,2,6};
    static const VkBorderColor borders[6]={VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        VK_BORDER_COLOR_INT_TRANSPARENT_BLACK,VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        VK_BORDER_COLOR_INT_OPAQUE_BLACK,VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        VK_BORDER_COLOR_INT_OPAQUE_WHITE};
    for(unsigned axes=0;axes<64;++axes)for(unsigned filters=0;filters<4;++filters)
        for(unsigned mip=0;mip<2;++mip)for(unsigned border=0;border<6;++border) {
            unsigned ui=axes&3,vi=(axes>>2)&3,wi=(axes>>4)&3;
            VkSamplerCreateInfo variant={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter=(filters&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
                .minFilter=(filters&2)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
                .mipmapMode=mip?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU=modes[ui],.addressModeV=modes[vi],.addressModeW=modes[wi],
                .borderColor=borders[border]};
            VkSampler candidate;
            assert(vkCreateSampler(&d,&variant,NULL,&candidate)==VK_SUCCESS);
            uint32_t descriptor[12];
            assert(ps5vk_texture_descriptor(&d,view,candidate,descriptor)==VK_SUCCESS);
            assert(!memcmp(descriptor,words,8*sizeof(uint32_t)));
            assert(descriptor[8]==(native_modes[ui]|(native_modes[vi]<<3)|(native_modes[wi]<<6)));
            assert(!descriptor[9]);
            assert(descriptor[10]==((filters&1?1u<<20:0u)|(filters&2?1u<<22:0u)|
                ((mip?2u:1u)<<26)));
            assert(descriptor[11]==((border/2u)<<30));
            vkDestroySampler(&d,candidate,NULL);
            assert(d.sampler_objects==1); /* Original fixture stays alive. */
        }
    /* The GPL ps5-opengl descriptor contract is exposed through Vulkan image
     * and view semantics, including array subsets, one cube and 3D slices. */
    d.max_allocation=16384;
    VkMemoryAllocateInfo layered_ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=8192};
    VkDeviceMemory layered_memory;
    assert(vkAllocateMemory(&d,&layered_ai,NULL,&layered_memory)==VK_SUCCESS);
    VkImageCreateInfo layered_ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={64,4,1},.mipLevels=1,.arrayLayers=3,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|
            VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage layered_image;
    assert(vkCreateImage(&d,&layered_ii,NULL,&layered_image)==VK_SUCCESS &&
        layered_image->requirements.size==3072);
    assert(vkBindImageMemory(&d,layered_image,layered_memory,0)==VK_SUCCESS);
    VkImageViewCreateInfo layered_vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=layered_image,.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY,.format=layered_ii.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,1,2}};
    VkImageView layered_view;
    assert(vkCreateImageView(&d,&layered_vi,NULL,&layered_view)==VK_SUCCESS);
    uint32_t layered_words[12];
    assert(ps5vk_texture_descriptor(&d,layered_view,sampler,layered_words)==VK_SUCCESS);
    void *layered_base;VkDeviceSize layered_bytes;
    assert(ps5vk_image_span(&d,layered_image,&layered_base,&layered_bytes)==VK_SUCCESS);
    assert(layered_words[0]==(uint32_t)((uintptr_t)layered_base>>8) &&
        layered_words[3]==0xd0000fac && layered_words[4]==0x00010002);
    vkDestroyImageView(&d,layered_view,NULL);
    layered_vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    layered_vi.subresourceRange.baseArrayLayer=2;
    layered_vi.subresourceRange.layerCount=1;
    assert(vkCreateImageView(&d,&layered_vi,NULL,&layered_view)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,layered_view,sampler,layered_words)==VK_SUCCESS &&
        layered_words[0]==(uint32_t)(((uintptr_t)layered_base+2048)>>8) &&
        layered_words[3]==0x90000fac && !layered_words[4]);
    vkDestroyImageView(&d,layered_view,NULL);
    vkDestroyImage(&d,layered_image,NULL);

    layered_ii.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    layered_ii.extent=(VkExtent3D){4,4,1};layered_ii.arrayLayers=6;
    assert(vkCreateImage(&d,&layered_ii,NULL,&layered_image)==VK_SUCCESS &&
        layered_image->requirements.size==6144);
    assert(vkBindImageMemory(&d,layered_image,layered_memory,0)==VK_SUCCESS);
    layered_vi=(VkImageViewCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=layered_image,.viewType=VK_IMAGE_VIEW_TYPE_CUBE,.format=layered_ii.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6}};
    assert(vkCreateImageView(&d,&layered_vi,NULL,&layered_view)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,layered_view,sampler,layered_words)==VK_SUCCESS &&
        layered_words[3]==0xb0000fac && !layered_words[4]);
    vkDestroyImageView(&d,layered_view,NULL);vkDestroyImage(&d,layered_image,NULL);

    layered_ii.flags=0;layered_ii.imageType=VK_IMAGE_TYPE_3D;
    layered_ii.extent=(VkExtent3D){4,4,3};layered_ii.arrayLayers=1;
    assert(vkCreateImage(&d,&layered_ii,NULL,&layered_image)==VK_SUCCESS &&
        layered_image->requirements.size==3072);
    assert(vkBindImageMemory(&d,layered_image,layered_memory,0)==VK_SUCCESS);
    layered_vi=(VkImageViewCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=layered_image,.viewType=VK_IMAGE_VIEW_TYPE_3D,.format=layered_ii.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    assert(vkCreateImageView(&d,&layered_vi,NULL,&layered_view)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,layered_view,sampler,layered_words)==VK_SUCCESS &&
        layered_words[3]==0xa0000fac && layered_words[4]==2);
    vkDestroyImageView(&d,layered_view,NULL);vkDestroyImage(&d,layered_image,NULL);

    layered_ii.imageType=VK_IMAGE_TYPE_1D;
    layered_ii.extent=(VkExtent3D){64,1,1};layered_ii.arrayLayers=3;
    assert(vkCreateImage(&d,&layered_ii,NULL,&layered_image)==VK_SUCCESS &&
        layered_image->requirements.size==768);
    assert(vkBindImageMemory(&d,layered_image,layered_memory,0)==VK_SUCCESS);
    layered_vi=(VkImageViewCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=layered_image,.viewType=VK_IMAGE_VIEW_TYPE_1D_ARRAY,.format=layered_ii.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,1,2}};
    assert(vkCreateImageView(&d,&layered_vi,NULL,&layered_view)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,layered_view,sampler,layered_words)==VK_SUCCESS &&
        layered_words[3]==0xc0000fac && layered_words[4]==0x00010002);
    vkDestroyImageView(&d,layered_view,NULL);
    layered_vi.viewType=VK_IMAGE_VIEW_TYPE_1D;
    layered_vi.subresourceRange.baseArrayLayer=2;
    layered_vi.subresourceRange.layerCount=1;
    assert(vkCreateImageView(&d,&layered_vi,NULL,&layered_view)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,layered_view,sampler,layered_words)==VK_SUCCESS &&
        layered_words[0]==(uint32_t)(((uintptr_t)layered_base+512)>>8) &&
        layered_words[3]==0x80000fac && !layered_words[4]);
    vkDestroyImageView(&d,layered_view,NULL);vkDestroyImage(&d,layered_image,NULL);
    vkFreeMemory(&d,layered_memory,NULL);
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

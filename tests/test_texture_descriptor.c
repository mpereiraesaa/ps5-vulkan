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
    assert(!words[4] && words[5]==0x400000 && !words[6] && !words[7]);
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
        .allocationSize=16384};
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
        layered_words[3]==0xb0000fac && layered_words[4]==5);
    vkDestroyImageView(&d,layered_view,NULL);vkDestroyImage(&d,layered_image,NULL);

    /* GFX10 cube resources select faces through DEPTH (last accessible layer)
     * and BASE_ARRAY in word 4. A cube-array view keeps the allocation base;
     * its cube/face range, including a view of only the second cube, belongs
     * in those fields rather than in a byte-address adjustment. */
    layered_ii.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    layered_ii.extent=(VkExtent3D){4,4,1};
    layered_ii.arrayLayers=12;
    assert(vkCreateImage(&d,&layered_ii,NULL,&layered_image)==VK_SUCCESS &&
        layered_image->requirements.size==12288);
    assert(vkBindImageMemory(&d,layered_image,layered_memory,0)==VK_SUCCESS);
    void *cube_array_base;VkDeviceSize cube_array_bytes;
    assert(ps5vk_image_span(&d,layered_image,&cube_array_base,&cube_array_bytes)==VK_SUCCESS &&
        cube_array_bytes==12288);
    d.enabled_features|=PS5VK_FEATURE_IMAGE_CUBE_ARRAY;
    layered_vi=(VkImageViewCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=layered_image,.viewType=VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,.format=layered_ii.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,12}};
    assert(vkCreateImageView(&d,&layered_vi,NULL,&layered_view)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,layered_view,sampler,layered_words)==VK_SUCCESS &&
        layered_words[3]==0xb0000fac && layered_words[4]==11);
    layered_vi.subresourceRange.baseArrayLayer=6;
    layered_vi.subresourceRange.layerCount=6;
    VkImageView second_cube_view;
    assert(vkCreateImageView(&d,&layered_vi,NULL,&second_cube_view)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,second_cube_view,sampler,layered_words)==VK_SUCCESS &&
        layered_words[0]==(uint32_t)((uintptr_t)cube_array_base>>8) &&
        layered_words[3]==0xb0000fac && layered_words[4]==0x0006000b);
    vkDestroyImageView(&d,second_cube_view,NULL);
    layered_vi.subresourceRange.baseArrayLayer=1;
    assert(vkCreateImageView(&d,&layered_vi,NULL,&second_cube_view)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,second_cube_view,sampler,layered_words)==VK_SUCCESS &&
        layered_words[0]==(uint32_t)((uintptr_t)cube_array_base>>8) &&
        layered_words[4]==0x00010006);
    vkDestroyImageView(&d,second_cube_view,NULL);
    d.enabled_features&=~(uint32_t)PS5VK_FEATURE_IMAGE_CUBE_ARRAY;
    assert(ps5vk_texture_descriptor(&d,layered_view,sampler,layered_words)==
        VK_ERROR_FEATURE_NOT_PRESENT);
    d.enabled_features|=PS5VK_FEATURE_IMAGE_CUBE_ARRAY;
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

    layered_ii=(VkImageCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={64,4,1},.mipLevels=3,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|
            VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    assert(vkCreateImage(&d,&layered_ii,NULL,&layered_image)==VK_SUCCESS &&
        layered_image->requirements.size==1792);
    assert(vkBindImageMemory(&d,layered_image,layered_memory,0)==VK_SUCCESS);
    layered_vi=(VkImageViewCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=layered_image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=layered_ii.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,3,0,1}};
    assert(vkCreateImageView(&d,&layered_vi,NULL,&layered_view)==VK_SUCCESS);
    VkSamplerCreateInfo mip_si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR,.maxLod=2.0f};
    VkSampler mip_sampler;assert(vkCreateSampler(&d,&mip_si,NULL,&mip_sampler)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,layered_view,mip_sampler,layered_words)==VK_SUCCESS &&
        layered_words[3]==0x90020fac && layered_words[5]==0x00400020 &&
        layered_words[9]==0x00200000 && layered_words[10]==0x08000000);
    vkDestroySampler(&d,mip_sampler,NULL);
    mip_si.mipLodBias=-2.0f;
    assert(vkCreateSampler(&d,&mip_si,NULL,&mip_sampler)==VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d,layered_view,mip_sampler,layered_words)==VK_SUCCESS &&
        layered_words[10]==0x08003e00);
    vkDestroySampler(&d,mip_sampler,NULL);
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
    /* Resource-only input-attachment descriptor: the same GFX10 image fields,
     * eight DWORDs and no sampler words at all. */
    {
        /* Six layers of a 64x64 RGBA8 attachment are 128 KiB each, so the
         * fixture has to raise the device's per-allocation ceiling. */
        d.max_allocation=1u<<21;
        VkImageCreateInfo input_ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .extent={64,64,1},.mipLevels=1,.arrayLayers=6,
            .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImage input_image;assert(vkCreateImage(&d,&input_ii,NULL,&input_image)==VK_SUCCESS);
        VkDeviceMemory input_memory;
        VkMemoryAllocateInfo input_ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=input_image->requirements.size};
        assert(vkAllocateMemory(&d,&input_ai,NULL,&input_memory)==VK_SUCCESS);
        assert(vkBindImageMemory(&d,input_image,input_memory,0)==VK_SUCCESS);
        VkImageViewCreateInfo input_vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image=input_image,.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY,.format=input_ii.format,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,1,4}};
        VkImageView input_view;assert(vkCreateImageView(&d,&input_vi,NULL,&input_view)==VK_SUCCESS);

        void *input_base;VkDeviceSize input_bytes;
        assert(ps5vk_image_span(&d,input_image,&input_base,&input_bytes)==VK_SUCCESS);
        uint32_t input_words[8];assert(ps5vk_image_resource_descriptor(&d,input_view,input_words)==VK_SUCCESS);
        /* Word identity: address, format, dimensions, the tiled 2D_ARRAY type
         * word, the layer range of the view and the single-mip encoding. The
         * attachment is SW_64K_R_X, so a padded-linear resource word would be
         * a valid address interpreted through the wrong pixel equation. */
        assert(input_words[0]==(uint32_t)((uintptr_t)input_base>>8));
        assert(input_words[1]==((uint32_t)((uintptr_t)input_base>>40)|(56u<<20)|(((64u-1u)&3u)<<30)));
        assert(input_words[2]==((63u>>2)|(63u<<14)|(1u<<31)));
        assert(input_words[3]==(0x01b00facu|(13u<<28)|(0u<<12)|((0u+1u-1u)<<16)));
        assert(input_words[4]==((1u<<16)|4u));
        assert(input_words[5]==0x400000);
        /* The eight words are the whole record: nothing beyond them is written,
         * and no sampler is read (the entry point has no sampler parameter). */
        uint32_t guard[10];for(unsigned i=0;i<10;++i)guard[i]=0xa5a5a5a5u;
        assert(ps5vk_image_resource_descriptor(&d,input_view,guard)==VK_SUCCESS);
        assert(guard[8]==0xa5a5a5a5u && guard[9]==0xa5a5a5a5u &&
            !memcmp(guard,input_words,8*sizeof(uint32_t)));

        /* Fail-closed edges: each leaves the caller's words untouched. */
        uint32_t before[8];memcpy(before,input_words,sizeof(before));
        uint32_t sink[8];for(unsigned i=0;i<8;++i)sink[i]=0xcafecafeu;
        uint32_t expected[8];memcpy(expected,sink,sizeof(sink));
        /* 1. A sampled-only image is not an input-attachment resource, even
         * though the same view shape over it encodes fine as a sampled one. */
        VkImageViewCreateInfo sampled_vi=input_vi;sampled_vi.image=image;
        sampled_vi.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView sampled_view;assert(vkCreateImageView(&d,&sampled_vi,NULL,&sampled_view)==VK_SUCCESS);
        assert(ps5vk_image_resource_descriptor(&d,sampled_view,sink)==VK_ERROR_FEATURE_NOT_PRESENT &&
            !memcmp(sink,expected,sizeof(sink)));
        /* 2. A single-layer 2D view of the same image is accepted, and it
         * emits the 2D type word with no layer range. */
        VkImageViewCreateInfo single_vi=input_vi;
        single_vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        single_vi.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView single_view;assert(vkCreateImageView(&d,&single_vi,NULL,&single_view)==VK_SUCCESS);
        assert(ps5vk_image_resource_descriptor(&d,single_view,sink)==VK_SUCCESS &&
            sink[3]==0x91b00facu && !sink[4] && !memcmp(sink,input_words,3*sizeof(uint32_t)));
        memcpy(sink,expected,sizeof(sink));
        /* 2b. A MULTISAMPLED colour attachment is the same record with the
         * sample geometry in the LEVEL fields (DXVK262-T06): BASE_LEVEL stays
         * zero and LAST_LEVEL names log2(samples), exactly as the pinned
         * compiler describes a multisampled texture to this hardware. */
        {
            d.platform_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;
            VkImageCreateInfo ms_ii = input_ii;
            ms_ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ms_ii.samples = VK_SAMPLE_COUNT_4_BIT;
            ms_ii.arrayLayers = 1;
            ms_ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            VkImage ms_image;
            assert(vkCreateImage(&d, &ms_ii, NULL, &ms_image) == VK_SUCCESS);
            VkMemoryAllocateInfo ms_ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = ms_image->requirements.size};
            VkDeviceMemory ms_memory;
            assert(vkAllocateMemory(&d, &ms_ai, NULL, &ms_memory) == VK_SUCCESS);
            assert(vkBindImageMemory(&d, ms_image, ms_memory, 0) == VK_SUCCESS);
            VkImageViewCreateInfo ms_vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = ms_image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkImageView ms_view;
            assert(vkCreateImageView(&d, &ms_vi, NULL, &ms_view) == VK_SUCCESS);
            uint32_t ms_words[8];
            assert(ps5vk_image_resource_descriptor(&d, ms_view, ms_words) == VK_SUCCESS);
            assert((ms_words[3] & UINT32_C(0x000ff000)) == (2u << 16));
            /* The ARRAY MSAA type tag the pinned compiler's own mapping gives
             * a subpassInputMS - not the plain 2D one and not the non-array
             * MSAA one - with the single layer described by a zero depth
             * field. */
            assert((ms_words[3] >> 28) == 15u);
            assert(ms_words[4] == 0u);
            /* MAX_MIP carries the same sample geometry the pinned GFX9 path
             * writes for a multisampled surface. */
            assert(((ms_words[5] >> 4) & 0xf) == 2u);
            /* The same record for the shape the pinned CTS uses to RESOLVE: an
             * image that declares COLOR_ATTACHMENT and TRANSFER_SRC but NOT
             * the input-attachment role (RENDER_TYPE_RESOLVE,
             * vktPipelineMultisampleTests.cpp). A subpass resolve is
             * implementation work rather than an application's descriptor read,
             * so the app-facing input-attachment gate keeps requiring the role
             * while the driver's own resolve draw may read the attachment the
             * pass declares - measured: without this the min_sample_shading
             * triangle leaves failed at vkQueueSubmit from the resolve
             * emission's descriptor build. */
            VkImageCreateInfo resolve_ii = ms_ii;
            resolve_ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            VkImage resolve_image;
            assert(vkCreateImage(&d, &resolve_ii, NULL, &resolve_image) == VK_SUCCESS);
            VkMemoryAllocateInfo resolve_ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = resolve_image->requirements.size};
            VkDeviceMemory resolve_memory;
            assert(vkAllocateMemory(&d, &resolve_ai, NULL, &resolve_memory) == VK_SUCCESS);
            assert(vkBindImageMemory(&d, resolve_image, resolve_memory, 0) == VK_SUCCESS);
            VkImageViewCreateInfo resolve_vi = ms_vi;
            resolve_vi.image = resolve_image;
            VkImageView resolve_view;
            assert(vkCreateImageView(&d, &resolve_vi, NULL, &resolve_view) == VK_SUCCESS);
            uint32_t resolve_words[8];
            assert(ps5vk_image_resource_descriptor(&d, resolve_view, resolve_words) == VK_SUCCESS);
            assert((resolve_words[3] >> 28) == 15u);
            /* ... and an image without the colour-attachment role stays
             * refused: the read this record describes is a render target read. */
            VkImageCreateInfo sampled_ii = resolve_ii;
            sampled_ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            VkImage sampled_image;
            assert(vkCreateImage(&d, &sampled_ii, NULL, &sampled_image) ==
                   VK_ERROR_FEATURE_NOT_PRESENT);
            vkDestroyImageView(&d, resolve_view, NULL);
            vkDestroyImage(&d, resolve_image, NULL);
            vkFreeMemory(&d, resolve_memory, NULL);
            /* The single-sample record for the same shape carries no sample
             * geometry at all, which is the difference the fields express. */
            VkImageCreateInfo single_ii = ms_ii;
            single_ii.samples = VK_SAMPLE_COUNT_1_BIT;
            single_ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            VkImage single_ms;
            assert(vkCreateImage(&d, &single_ii, NULL, &single_ms) == VK_SUCCESS);
            VkMemoryAllocateInfo single_ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = single_ms->requirements.size};
            VkDeviceMemory single_memory;
            assert(vkAllocateMemory(&d, &single_ai, NULL, &single_memory) == VK_SUCCESS);
            assert(vkBindImageMemory(&d, single_ms, single_memory, 0) == VK_SUCCESS);
            VkImageViewCreateInfo single_ms_vi = ms_vi;
            single_ms_vi.image = single_ms;
            VkImageView single_ms_view;
            assert(vkCreateImageView(&d, &single_ms_vi, NULL, &single_ms_view) == VK_SUCCESS);
            uint32_t plain_words[8];
            assert(ps5vk_image_resource_descriptor(&d, single_ms_view, plain_words) == VK_SUCCESS);
            assert(!(plain_words[3] & UINT32_C(0x000ff000)));
            /* A count this profile does not implement stays refused. */
            ms_ii.samples = VK_SAMPLE_COUNT_8_BIT;
            VkImage eight;
            assert(vkCreateImage(&d, &ms_ii, NULL, &eight) != VK_SUCCESS);
            vkDestroyImageView(&d, single_ms_view, NULL);
            vkDestroyImage(&d, single_ms, NULL);
            vkFreeMemory(&d, single_memory, NULL);
            vkDestroyImageView(&d, ms_view, NULL);
            vkDestroyImage(&d, ms_image, NULL);
            vkFreeMemory(&d, ms_memory, NULL);
            d.platform_features &= ~(uint32_t)PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        }
        /* 3. A 1D view type, a cube view type and a 3D view type are refused. */
        VkImageViewCreateInfo plain=input_vi;plain.viewType=VK_IMAGE_VIEW_TYPE_2D;
        plain.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView plain_view;assert(vkCreateImageView(&d,&plain,NULL,&plain_view)==VK_SUCCESS);
        plain_view->view_type=VK_IMAGE_VIEW_TYPE_1D;
        assert(ps5vk_image_resource_descriptor(&d,plain_view,sink)==VK_ERROR_FEATURE_NOT_PRESENT &&
            !memcmp(sink,expected,sizeof(sink)));
        plain_view->view_type=VK_IMAGE_VIEW_TYPE_CUBE;
        assert(ps5vk_image_resource_descriptor(&d,plain_view,sink)==VK_ERROR_FEATURE_NOT_PRESENT &&
            !memcmp(sink,expected,sizeof(sink)));
        plain_view->view_type=VK_IMAGE_VIEW_TYPE_3D;
        assert(ps5vk_image_resource_descriptor(&d,plain_view,sink)==VK_ERROR_FEATURE_NOT_PRESENT &&
            !memcmp(sink,expected,sizeof(sink)));
        plain_view->view_type=VK_IMAGE_VIEW_TYPE_2D;
        /* 4. Another view format, another device and a missing view/image fail
         * closed. The API refuses a mismatched view format outright, so the
         * encoder's own check is probed directly. */
        plain_view->format=VK_FORMAT_R8G8B8A8_SNORM;
        assert(ps5vk_image_resource_descriptor(&d,plain_view,sink)==VK_ERROR_FEATURE_NOT_PRESENT &&
            !memcmp(sink,expected,sizeof(sink)));
        plain_view->format=input_ii.format;
        struct VkDevice_T other={0};
        assert(ps5vk_image_resource_descriptor(&other,input_view,sink)==VK_ERROR_UNKNOWN &&
            !memcmp(sink,expected,sizeof(sink)));
        assert(ps5vk_image_resource_descriptor(&d,NULL,sink)==VK_ERROR_UNKNOWN &&
            !memcmp(sink,expected,sizeof(sink)));
        VkImageView orphan=input_view;VkImage saved_image=orphan->image;orphan->image=NULL;
        assert(ps5vk_image_resource_descriptor(&d,orphan,sink)==VK_ERROR_UNKNOWN &&
            !memcmp(sink,expected,sizeof(sink)));
        orphan->image=saved_image;
        /* 5. An image over the witnessed six-layer ceiling, and an unbound one. */
        struct VkImage_T seven={0};seven.device=&d;
        seven.info=input_ii;seven.info.arrayLayers=7;
        struct VkImageView_T seven_view={0};seven_view.device=&d;seven_view.image=&seven;
        seven_view.view_type=VK_IMAGE_VIEW_TYPE_2D_ARRAY;seven_view.format=input_ii.format;
        seven_view.range=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,7};
        assert(ps5vk_image_resource_descriptor(&d,&seven_view,sink)==VK_ERROR_FEATURE_NOT_PRESENT &&
            !memcmp(sink,expected,sizeof(sink)));
        VkImage unbound;assert(vkCreateImage(&d,&input_ii,NULL,&unbound)==VK_SUCCESS);
        struct VkImageView_T unbound_view={0};unbound_view.device=&d;unbound_view.image=unbound;
        unbound_view.view_type=VK_IMAGE_VIEW_TYPE_2D_ARRAY;unbound_view.format=input_ii.format;
        unbound_view.range=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6};
        assert(ps5vk_image_resource_descriptor(&d,&unbound_view,sink)==VK_ERROR_UNKNOWN &&
            !memcmp(sink,expected,sizeof(sink)));
        vkDestroyImage(&d,unbound,NULL);
        /* The accepted record is stable across repeated calls. */
        assert(ps5vk_image_resource_descriptor(&d,input_view,input_words)==VK_SUCCESS &&
            !memcmp(input_words,before,sizeof(before)));
        vkDestroyImageView(&d,sampled_view,NULL);vkDestroyImageView(&d,single_view,NULL);
        vkDestroyImageView(&d,plain_view,NULL);
        vkDestroyImageView(&d,input_view,NULL);vkDestroyImage(&d,input_image,NULL);
        vkFreeMemory(&d,input_memory,NULL);
    }

    vkDestroySampler(&d,sampler,NULL);vkDestroyImageView(&d,view,NULL);
    vkDestroyImage(&d,image,NULL);vkFreeMemory(&d,memory,NULL);assert(!d.graphics_objects);
}

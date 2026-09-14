#include "vk_command.h"
#include "vk_image.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static VkResult allocate(void *ctx,VkDeviceSize n,void **address,void **backing)
{ (void)ctx;*address=calloc(1,(size_t)n);*backing=*address;return *address?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release(void *ctx,void *backing){(void)ctx;free(backing);}
static VkResult cache(void *ctx,void *backing,VkDeviceSize offset,VkDeviceSize size)
{(void)ctx;(void)backing;(void)offset;(void)size;return VK_SUCCESS;}

int main(void)
{
    struct VkDevice_T d={.graphics_enabled=VK_TRUE,
        .memory={NULL,allocate,release,cache,cache},.buffer_alignment=256,
        .noncoherent_atom=64,.max_allocation=1024*1024};
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=512*1024};
    VkDeviceMemory memory;assert(vkAllocateMemory(&d,&allocation,NULL,&memory)==VK_SUCCESS);
    struct VkImage_T image={.device=&d,.memory=memory,.requirements={.size=256*1024},
        .info={.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .extent={256,256,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
            .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT}};
    d.images=&image;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=256*256*4,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer destination;assert(vkCreateBuffer(&d,&bi,NULL,&destination)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,destination,memory,256*1024)==VK_SUCCESS);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;assert(vkCreateCommandPool(&d,&pi,NULL,&pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.commandBufferCount=1};
    VkCommandBuffer command;assert(vkAllocateCommandBuffers(&d,&ai,&command)==VK_SUCCESS);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={256,256,1}};
    assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    vkCmdCopyImageToBuffer(command,&image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination,1,&region);
    assert(command->state==PS5VK_RECORDING && command->operation_count==1 &&
        command->operations[0].type==PS5VK_COPY_IMAGE_BUFFER &&
        command->operations[0].copy_destination==destination);
    region.imageExtent.width=1;
    assert(command->operations[0].copy_region.imageExtent.width==256);

    assert(vkResetCommandBuffer(command,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    vkCmdCopyImageToBuffer(command,&image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination,1,&region);
    assert(command->state==PS5VK_INVALID && !command->operation_count);

    assert(vkResetCommandBuffer(command,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    region.imageExtent.width=256;region.bufferRowLength=255;
    vkCmdCopyImageToBuffer(command,&image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination,1,&region);
    assert(command->state==PS5VK_INVALID && !command->operation_count);

    /* The transfer-only role is host-visible memory, so a TRANSFER_SRC-only
     * image is a legal (write-less) source for the bounded readback and is
     * recorded as frontend work over the padded layout. */
    assert(vkResetCommandBuffer(command,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    region.bufferRowLength=0;image.info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vkCmdCopyImageToBuffer(command,&image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination,1,&region);
    assert(command->state==PS5VK_RECORDING && command->operation_count==1 &&
        command->operations[0].type==PS5VK_COPY_IMAGE_BUFFER);

    /* Roles outside both executable paths stay refused, here a sampled role
     * that is not the upload destination. */
    assert(vkResetCommandBuffer(command,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    image.info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vkCmdCopyImageToBuffer(command,&image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination,1,&region);
    assert(command->state==PS5VK_INVALID && !command->operation_count);

    /* Upload visibility must be recordable for vertex sampling, without
     * substituting ALL_COMMANDS or confusing vertex fetch with shader reads. */
    image.info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=&image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    const VkPipelineStageFlags readers[]={VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    for(unsigned n=0;n<sizeof(readers)/sizeof(readers[0]);++n) {
        assert(vkResetCommandBuffer(command,0)==VK_SUCCESS &&
               vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,readers[n],
            0,0,NULL,0,NULL,1,&barrier);
        assert(command->state==PS5VK_RECORDING && command->operation_count==1);
        assert(command->operations[0].type==PS5VK_IMAGE_BARRIER &&
               command->operations[0].dst_stage==readers[n] &&
               command->operations[0].image_barrier.image==&image &&
               command->operations[0].image_barrier.dstAccessMask==VK_ACCESS_SHADER_READ_BIT);
        assert(vkEndCommandBuffer(command)==VK_SUCCESS);
    }
    const VkPipelineStageFlags invalid_readers[]={0,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    for(unsigned n=0;n<sizeof(invalid_readers)/sizeof(invalid_readers[0]);++n) {
        assert(vkResetCommandBuffer(command,0)==VK_SUCCESS &&
               vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,invalid_readers[n],
            0,0,NULL,0,NULL,1,&barrier);
        assert(command->state==PS5VK_INVALID && !command->operation_count);
    }
    vkDestroyCommandPool(&d,pool,NULL);vkDestroyBuffer(&d,destination,NULL);
    d.images=NULL;vkFreeMemory(&d,memory,NULL);
    puts("Bounded image-to-buffer recording: pass (host only)");
}

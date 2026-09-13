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

    assert(vkResetCommandBuffer(command,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    region.bufferRowLength=0;image.info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vkCmdCopyImageToBuffer(command,&image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination,1,&region);
    assert(command->state==PS5VK_INVALID && !command->operation_count);

    vkDestroyCommandPool(&d,pool,NULL);vkDestroyBuffer(&d,destination,NULL);
    d.images=NULL;vkFreeMemory(&d,memory,NULL);
    puts("Bounded image-to-buffer recording: pass (host only)");
}

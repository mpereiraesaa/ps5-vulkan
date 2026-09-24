/* Host recording contract for the default-off RGBA8 integer gather readback. */
#include "vk_internal.h"
#include "vk_command.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{ (void)ctx; *address=*backing=calloc(1,(size_t)size); return *address?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY; }
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult sync_memory(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend=(struct ps5vk_memory_backend){NULL,alloc_memory,free_memory,sync_memory,sync_memory};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *platform)
{
    *platform=(struct ps5vk_platform){.open=open_backend,.close=close_backend,
        .max_allocation=1u<<20,.queue_flags=VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile={.name="host mock, not a GPU",
        .vendor_id=0x1002u,.heap_size=1u<<20,.allocation_granularity=1,
        .buffer_image_granularity=1};
    ps5vk_physical_profile_init(&platform->properties,&platform->memory_properties,&profile);
    return VK_SUCCESS;
}

static void integer_target(VkDevice device, VkCommandPool pool, VkFormat format)
{
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=format,.extent={64,64,1},.mipLevels=1,
        .arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkImage image=VK_NULL_HANDLE;
    assert(vkCreateImage(device,&image_info,NULL,&image)==VK_SUCCESS);
    VkMemoryRequirements image_req; vkGetImageMemoryRequirements(device,image,&image_req);
    VkMemoryAllocateInfo image_alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=image_req.size,.memoryTypeIndex=0};
    VkDeviceMemory image_memory=VK_NULL_HANDLE;
    assert(vkAllocateMemory(device,&image_alloc,NULL,&image_memory)==VK_SUCCESS);
    assert(vkBindImageMemory(device,image,image_memory,0)==VK_SUCCESS);
    assert(ps5vk_integer_colour_readback_image(image));
    void *image_address=NULL; VkDeviceSize image_bytes=0;
    assert(ps5vk_image_span(device,image,&image_address,&image_bytes)==VK_SUCCESS &&
           image_address && image_bytes>=64u*64u*4u);

    const VkDeviceSize byte_count=64u*64u*4u;
    VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=byte_count,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer=VK_NULL_HANDLE;
    assert(vkCreateBuffer(device,&buffer_info,NULL,&buffer)==VK_SUCCESS);
    VkMemoryRequirements buffer_req; vkGetBufferMemoryRequirements(device,buffer,&buffer_req);
    VkMemoryAllocateInfo buffer_alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=buffer_req.size,.memoryTypeIndex=0};
    VkDeviceMemory buffer_memory=VK_NULL_HANDLE;
    assert(vkAllocateMemory(device,&buffer_alloc,NULL,&buffer_memory)==VK_SUCCESS);
    assert(vkBindBufferMemory(device,buffer,buffer_memory,0)==VK_SUCCESS);

    VkCommandBufferAllocateInfo command_alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command=VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device,&command_alloc,&command)==VK_SUCCESS);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    VkImageMemoryBarrier image_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&image_barrier);
    assert(command->state==PS5VK_RECORDING && command->operation_count==1);
    VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={64,64,1}};
    vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&region);
    assert(command->state==PS5VK_RECORDING && command->operation_count==2);
    VkBufferMemoryBarrier host_barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffer,
        .offset=0,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,NULL,1,&host_barrier,0,NULL);
    assert(command->state==PS5VK_RECORDING && command->operation_count==4);
    assert(vkEndCommandBuffer(command)==VK_SUCCESS);

    vkFreeCommandBuffers(device,pool,1,&command);
    vkDestroyBuffer(device,buffer,NULL); vkFreeMemory(device,buffer_memory,NULL);
    vkDestroyImage(device,image,NULL); vkFreeMemory(device,image_memory,NULL);
}

int main(void)
{
    VkInstance instance=VK_NULL_HANDLE;
    VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&instance_info,NULL,&instance)==VK_SUCCESS);
    uint32_t count=1; VkPhysicalDevice physical=VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(instance,&count,&physical)==VK_SUCCESS);
    float priority=1.0f;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info};
    VkDevice device=VK_NULL_HANDLE;
    assert(vkCreateDevice(physical,&device_info,NULL,&device)==VK_SUCCESS);
    device->graphics_enabled=VK_TRUE;
    device->image_requirements=ps5vk_native_image_requirements;
    VkCommandPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool=VK_NULL_HANDLE;
    assert(vkCreateCommandPool(device,&pool_info,NULL,&pool)==VK_SUCCESS);
    integer_target(device,pool,VK_FORMAT_R8G8B8A8_UINT);
    integer_target(device,pool,VK_FORMAT_R8G8B8A8_SINT);
    vkDestroyCommandPool(device,pool,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    puts("RGBA8 UINT/SINT CTS readback recording: pass (host only; no GPU execution)");
    return 0;
}

/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Public buffer/image transfers over padded mip and array-layer storage.
 */
#include "vk_internal.h"
#include "vk_command.h"
#include "vk_framebuffer.h"
#include "vk_image_transfer.h"
#include "physical_device_profile.h"
#include "texture_copy.h"
#include "texture_layout.h"
#include "graphics_formats.h"
#include "bc_blit_decode.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = calloc(1, (size_t)size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
struct cache_event { void *backing; VkDeviceSize offset,size; int invalidate; };
static struct cache_event events[32];
static unsigned event_count;
static VkResult cache_sync(void *backing,VkDeviceSize offset,VkDeviceSize size,int invalidate)
{
    assert(event_count<32);
    events[event_count++]=(struct cache_event){backing,offset,size,invalidate};
    return VK_SUCCESS;
}
static VkResult flush_sync(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; return cache_sync(backing,offset,size,0); }
static VkResult invalidate_sync(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; return cache_sync(backing,offset,size,1); }
static void expect_cache(void *backing,VkDeviceSize offset,VkDeviceSize size,int invalidate)
{
    for(unsigned i=0;i<event_count;++i)
        if(events[i].backing==backing && events[i].offset==offset &&
           events[i].size==size && events[i].invalidate==invalidate)return;
    assert(!"missing exact cache span");
}
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, flush_sync, invalidate_sync};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }

VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
                                 .max_allocation = 1u << 20,
                                 .queue_flags = VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU",
        .vendor_id = 0x1002u,
        .heap_size = 1u << 20,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static VkDevice device;
static VkDeviceMemory last_memory;
static VkCommandPool pool;
enum { WIDTH = 8, HEIGHT = 4 };

static VkImage make_image_subresources(VkFormat format, VkImageUsageFlags usage,
    uint32_t width, uint32_t height, uint32_t mips, uint32_t layers, VkImageTiling tiling, void **mapped)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {width, height, 1},
                              .mipLevels = mips, .arrayLayers = layers,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = tiling,
                              .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage image;
    VkResult created=vkCreateImage(device, &info, NULL, &image);
    if(created) fprintf(stderr,"create format %u result %d\n",format,created);
    assert(created == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = requirements.size, .memoryTypeIndex = 0u};
    VkDeviceMemory memory;
    assert(vkAllocateMemory(device, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS);
    if (mapped) assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS);
    return image;
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer c;
    assert(vkAllocateCommandBuffers(device, &ai, &c) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(c, &bi) == VK_SUCCESS);
    return c;
}

static VkBuffer make_buffer(VkBufferUsageFlags usage, VkDeviceSize size, void **mapped)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;
    assert(vkCreateBuffer(device, &info, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = requirements.size, .memoryTypeIndex = 0u};
    VkDeviceMemory memory;
    assert(vkAllocateMemory(device, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    last_memory=memory;
    if (mapped) assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS);
    return buffer;
}

static void submit_and_wait(VkCommandBuffer command)
{
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    assert(vkCreateFence(device, &fi, NULL, &fence) == VK_SUCCESS);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vkQueueSubmit(&device->queue, 1, &si, fence) == VK_SUCCESS);
    assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000ull) == VK_SUCCESS);
    vkDestroyFence(device, fence, NULL);
}


static void round_trip(VkFormat format, int general)
{
    enum { N = 4096 };
    const unsigned bw = ps5vk_texture_format_block_compressed(format) ? 4 : 1;
    struct ps5vk_texture_mip_layout layout;
    assert(!ps5vk_texture_mip_layout_for_slices(format, 17, 13, 3, 3, &layout));
    void *im, *up, *down;
    VkImage image = make_image_subresources(format,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        17, 13, 3, 3, VK_IMAGE_TILING_OPTIMAL, &im);
    VkBuffer upload = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, N, &up);
    VkDeviceMemory um=last_memory;
    VkBuffer download = make_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, N, &down);
    VkDeviceMemory dm=last_memory;
    memset(im, 0xa5, (size_t)layout.bytes);
    memset(down, 0x5a, N);
    for (unsigned i=0;i<N;++i) ((unsigned char *)up)[i]=(unsigned char)(i*29+7);
    VkBufferImageCopy regions[2] = {
        {.bufferOffset=32,.bufferRowLength=16,.bufferImageHeight=12,
         .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,1,1,2},
         .imageOffset={0,0,0},.imageExtent={8,6,1}},
        {.bufferOffset=2048,.bufferRowLength=8,.bufferImageHeight=4,
         .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,2,0,2},
         .imageOffset={0,0,0},.imageExtent={4,3,1}}};
    VkDeviceSize buffer_begin[2],buffer_size[2],image_begin[2],image_size[2];
    unsigned char *expected_image=malloc((size_t)layout.bytes), expected_buffer[N];
    assert(expected_image);memset(expected_image,0xa5,(size_t)layout.bytes);
    memset(expected_buffer,0x5a,N);
    /* Independent address oracle uses documented block geometry and mip layout. */
    const unsigned block_bytes=ps5vk_texture_format_lookup(format)->bytes_per_block;
    for(unsigned j=0;j<2;++j) {
        const VkBufferImageCopy *r=&regions[j];
        const unsigned rows=(r->imageExtent.height+bw-1)/bw;
        const unsigned bytes=((r->imageExtent.width+bw-1)/bw)*block_bytes;
        const size_t pitch=r->bufferRowLength/bw*block_bytes;
        const size_t slice=pitch*(r->bufferImageHeight/bw);
        buffer_begin[j]=r->bufferOffset;
        buffer_size[j]=(r->imageSubresource.layerCount-1)*slice+(rows-1)*pitch+bytes;
        image_begin[j]=r->imageSubresource.baseArrayLayer*layout.layer_stride+
            layout.levels[r->imageSubresource.mipLevel].offset;
        image_size[j]=(r->imageSubresource.layerCount-1)*layout.layer_stride+
            (rows-1)*layout.levels[r->imageSubresource.mipLevel].row_pitch+bytes;
        for(unsigned z=0;z<r->imageSubresource.layerCount;++z)
        for(unsigned y=0;y<rows;++y) {
            size_t a=r->bufferOffset+z*slice+y*pitch;
            size_t b=(r->imageSubresource.baseArrayLayer+z)*layout.layer_stride+
                layout.levels[r->imageSubresource.mipLevel].offset+
                y*layout.levels[r->imageSubresource.mipLevel].row_pitch;
            memcpy(expected_image+b,(unsigned char *)up+a,bytes);
            memcpy(expected_buffer+a,(unsigned char *)up+a,bytes);
        }
    }
    /* Barrier coverage is a separate contract. Set the completed layout so
     * this test isolates the public copy recording, routing and execution. */
    image->layout=general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkCommandBuffer c=begin();
    vkCmdCopyBufferToImage(c,upload,image,image->layout,2,regions);
    assert(c->state==PS5VK_RECORDING && c->operation_count==2);
    assert(ps5vk_image_domain(&c->operations[0])==PS5VK_IMAGE_DOMAIN_LINEAR);
    event_count=0;submit_and_wait(c);
    for(unsigned j=0;j<2;++j) {
        expect_cache(up,buffer_begin[j],buffer_size[j],1);
        expect_cache(im,image_begin[j],image_size[j],0);
    }
    assert(!memcmp(im,expected_image,(size_t)layout.bytes));
    image->layout=general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    c=begin();
    vkCmdCopyImageToBuffer(c,image,image->layout,download,2,regions);
    assert(c->state==PS5VK_RECORDING && c->operation_count==2);
    event_count=0;submit_and_wait(c);
    for(unsigned j=0;j<2;++j) {
        expect_cache(im,image_begin[j],image_size[j],1);
        expect_cache(down,buffer_begin[j],buffer_size[j],1);
        expect_cache(down,buffer_begin[j],buffer_size[j],0);
    }
    assert(!memcmp(down,expected_buffer,N));
    VkBufferImageCopy bad[2]={regions[0],regions[1]};
    bad[1].imageSubresource.baseArrayLayer=3;
    c=begin();vkCmdCopyImageToBuffer(c,image,image->layout,download,2,bad);
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    bad[1]=regions[1];bad[1].bufferOffset=N-4;
    c=begin();vkCmdCopyImageToBuffer(c,image,image->layout,download,2,bad);
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    c=begin();vkCmdCopyBufferToImage(c,upload,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,2,bad);
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    free(expected_image);
    VkDeviceMemory mem=image->memory;
    vkDestroyImage(device,image,NULL);vkDestroyBuffer(device,upload,NULL);
    vkDestroyBuffer(device,download,NULL);
    vkFreeMemory(device,mem,NULL);vkFreeMemory(device,um,NULL);vkFreeMemory(device,dm,NULL);
}
int main(void)
{
    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    device->graphics_enabled = VK_TRUE; /* the native platform installs these during configure */
    device->image_requirements = ps5vk_native_image_requirements;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &pci, NULL, &pool) == VK_SUCCESS);
    for(int general=0;general<2;++general) {
        for(VkFormat f=VK_FORMAT_BC1_RGB_UNORM_BLOCK;f<=VK_FORMAT_BC7_SRGB_BLOCK;++f)
            round_trip(f,general);
        round_trip(VK_FORMAT_R8G8B8A8_UNORM,general);
        round_trip(VK_FORMAT_R8G8B8A8_SRGB,general);
    }
    vkDestroyCommandPool(device,pool,NULL);
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("BC and RGBA subresource copies: pass");return 0;
}

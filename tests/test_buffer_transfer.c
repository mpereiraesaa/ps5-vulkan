#include "vk_queue.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct cache_counts { unsigned flushes, invalidates; };
struct backend_job { uint64_t serial; };
static unsigned launches, releases;

static VkResult allocate(void *context, VkDeviceSize size, void **address, void **backing)
{
    (void)context;
    *address = calloc(1, (size_t)size);
    *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void release(void *context, void *backing)
{ (void)context; free(backing); }
static VkResult flush(void *context, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    (void)backing; (void)offset; assert(size); ++((struct cache_counts *)context)->flushes;
    return VK_SUCCESS;
}
static VkResult invalidate(void *context, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    (void)backing; (void)offset; assert(size); ++((struct cache_counts *)context)->invalidates;
    return VK_SUCCESS;
}
static VkResult prepare(VkDevice d, const struct ps5vk_submission *submission, void **out)
{
    (void)d;
    struct backend_job *job=malloc(sizeof(*job));
    if(!job)return VK_ERROR_OUT_OF_HOST_MEMORY;
    job->serial=submission->serial;*out=job;return VK_SUCCESS;
}
static VkResult launch(VkDevice d, void *opaque)
{ (void)d;(void)opaque;++launches;return VK_SUCCESS; }
static VkResult poll(VkDevice d, void *opaque, uint64_t *completed)
{ (void)d;*completed=((struct backend_job *)opaque)->serial;return VK_SUCCESS; }
static void backend_release(VkDevice d, void *opaque)
{ (void)d;++releases;free(opaque); }
static VkBuffer buffer(VkDevice d, VkBufferUsageFlags usage, VkDeviceMemory memory)
{
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=67,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer value;assert(vkCreateBuffer(d,&info,NULL,&value)==VK_SUCCESS);
    assert(vkBindBufferMemory(d,value,memory,0)==VK_SUCCESS);return value;
}
static VkCommandBuffer command(VkDevice d, VkCommandPool *pool)
{
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(d,&pi,NULL,pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=*pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer value;assert(vkAllocateCommandBuffers(d,&ai,&value)==VK_SUCCESS);return value;
}

int main(void)
{
    struct cache_counts counts={0};
    struct VkDevice_T d={.memory={&counts,allocate,release,flush,invalidate},
        .buffer_alignment=4,.noncoherent_atom=4,.max_allocation=1024};
    d.queue.device=&d;d.queue.next_serial=1;
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=256,.memoryTypeIndex=0};
    VkDeviceMemory source_memory,destination_memory;
    assert(vkAllocateMemory(&d,&mi,NULL,&source_memory)==VK_SUCCESS);
    assert(vkAllocateMemory(&d,&mi,NULL,&destination_memory)==VK_SUCCESS);
    VkBuffer source=buffer(&d,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,source_memory);
    VkBuffer destination=buffer(&d,VK_BUFFER_USAGE_TRANSFER_DST_BIT,destination_memory);
    void *source_map,*destination_map;
    assert(vkMapMemory(&d,source_memory,0,VK_WHOLE_SIZE,0,&source_map)==VK_SUCCESS);
    assert(vkMapMemory(&d,destination_memory,0,VK_WHOLE_SIZE,0,&destination_map)==VK_SUCCESS);
    for(unsigned j=0;j<67;++j)((unsigned char *)source_map)[j]=(unsigned char)(j+1);
    memset(destination_map,0x5a,256);
    VkMappedMemoryRange mapped={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=source_memory,.offset=0,.size=VK_WHOLE_SIZE};
    assert(vkFlushMappedMemoryRanges(&d,1,&mapped)==VK_SUCCESS);

    VkCommandPool pool;
    VkCommandBuffer cmd=command(&d,&pool);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    VkBufferCopy copy={.srcOffset=1,.dstOffset=0,.size=7};
    vkCmdCopyBuffer(cmd,source,destination,1,&copy);
    uint32_t update[2]={0x11223344u,0xaabbccddu};
    vkCmdUpdateBuffer(cmd,destination,8,sizeof(update),update);
    update[0]=0;
    vkCmdFillBuffer(cmd,destination,16,16,0xdecafbad);
    vkCmdFillBuffer(cmd,destination,60,VK_WHOLE_SIZE,0x01020304);
    assert(cmd->state==PS5VK_RECORDING && cmd->operation_count==4);
    assert(vkEndCommandBuffer(cmd)==VK_SUCCESS);
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=1,.pCommandBuffers=&cmd};
    assert(vkQueueSubmit(&d.queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(!d.submission && cmd->state==PS5VK_EXECUTABLE);

    mapped.memory=destination_memory;
    assert(vkInvalidateMappedMemoryRanges(&d,1,&mapped)==VK_SUCCESS);
    unsigned char *bytes=destination_map;
    assert(!memcmp(bytes,(unsigned char[]){2,3,4,5,6,7,8},7));
    assert(bytes[7]==0x5a && ((uint32_t *)(bytes+8))[0]==0x11223344u &&
        ((uint32_t *)(bytes+8))[1]==0xaabbccddu);
    for(unsigned j=16;j<32;j+=4)assert(*(uint32_t *)(bytes+j)==0xdecafbadu);
    assert(*(uint32_t *)(bytes+60)==0x01020304u);
    assert(bytes[64]==0x5a && bytes[65]==0x5a && bytes[66]==0x5a && bytes[67]==0x5a);
    assert(counts.invalidates>=2 && counts.flushes>=5);

    /* Frontend transfers on both sides of a backend segment retain exact
     * command order. Backend prepare may happen early, but the trailing fill
     * cannot execute until the middle segment reports completion. */
    assert(vkResetCommandBuffer(cmd,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    uint32_t first=0x55667788u;
    vkCmdUpdateBuffer(cmd,destination,32,4,&first);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,0,NULL);
    vkCmdFillBuffer(cmd,destination,36,4,0x99aabbccu);
    assert(vkEndCommandBuffer(cmd)==VK_SUCCESS);
    d.submit_backend=(struct ps5vk_queue_backend){prepare,launch,poll,backend_release};
    assert(vkQueueSubmit(&d.queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(d.submission && launches==1 && releases==0);
    assert(*(uint32_t *)(bytes+32)==0x55667788u &&
        *(uint32_t *)(bytes+36)==0x5a5a5a5au);
    assert(ps5vk_queue_poll(&d)==VK_SUCCESS);
    assert(!d.submission && releases==1 && *(uint32_t *)(bytes+36)==0x99aabbccu);

    assert(vkResetCommandBuffer(cmd,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    VkBuffer both=buffer(&d,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,source_memory);
    copy=(VkBufferCopy){.srcOffset=0,.dstOffset=2,.size=4};
    vkCmdCopyBuffer(cmd,both,both,1,&copy);
    assert(cmd->state==PS5VK_INVALID && !cmd->operation_count);

    assert(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    VkBufferCopy destinations[]={
        {.srcOffset=0,.dstOffset=0,.size=4},
        {.srcOffset=8,.dstOffset=2,.size=4},
    };
    vkCmdCopyBuffer(cmd,source,destination,2,destinations);
    assert(cmd->state==PS5VK_INVALID && !cmd->operation_count);

    assert(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    vkCmdFillBuffer(cmd,destination,64,VK_WHOLE_SIZE,0);
    assert(cmd->state==PS5VK_RECORDING && !cmd->operation_count);

    assert(vkResetCommandBuffer(cmd,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    vkCmdUpdateBuffer(cmd,destination,2,4,&first);
    assert(cmd->state==PS5VK_INVALID && !cmd->operation_count);

    assert(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    vkCmdFillBuffer(cmd,destination,0,6,0);
    assert(cmd->state==PS5VK_INVALID && !cmd->operation_count);

    vkDestroyCommandPool(&d,pool,NULL);
    vkDestroyBuffer(&d,both,NULL);vkDestroyBuffer(&d,destination,NULL);
    vkDestroyBuffer(&d,source,NULL);
    vkUnmapMemory(&d,source_memory);vkUnmapMemory(&d,destination_memory);
    vkFreeMemory(&d,source_memory,NULL);vkFreeMemory(&d,destination_memory,NULL);
    puts("Ordered buffer copy/update/fill: pass (host frontend queue)");
}

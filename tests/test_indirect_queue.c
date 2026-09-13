#include "vk_indirect.h"
#include "vk_queue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct state {
    unsigned prepares,launches,polls,releases,invalidates;
    VkDispatchIndirectCommand *arguments;
    uint32_t resolved[3];
};
struct job { struct state *state;uint64_t serial;VkBool32 producer; };
static VkResult allocate(void *context,VkDeviceSize bytes,void **address,void **backing)
{ (void)context;*address=calloc(1,(size_t)bytes);*backing=*address;return *address?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release(void *context,void *backing)
{ (void)context;free(backing); }
static VkResult flush(void *context,void *backing,VkDeviceSize offset,VkDeviceSize bytes)
{ (void)context;(void)backing;(void)offset;(void)bytes;return VK_SUCCESS; }
static VkResult invalidate(void *context,void *backing,VkDeviceSize offset,VkDeviceSize bytes)
{ (void)backing;(void)offset;assert(bytes==12);++((struct state *)context)->invalidates;return VK_SUCCESS; }
static VkResult prepare(VkDevice d,const struct ps5vk_submission *submission,void **out)
{
    struct state *state=d->memory.context;++state->prepares;
    assert(submission->count==1);
    VkCommandBuffer command=submission->buffers[0];
    uint32_t first=ps5vk_submission_first_operation(submission,0);
    uint32_t count=ps5vk_submission_operation_count(submission,0);
    assert(count>=1);
    const struct ps5vk_operation *op=&command->operations[first];
    struct job *job=calloc(1,sizeof(*job));if(!job)return VK_ERROR_OUT_OF_HOST_MEMORY;
    job->state=state;job->serial=submission->serial;
    if(op->type==PS5VK_DISPATCH_INDIRECT) {
        struct ps5vk_operation resolved;
        VkResult rc=ps5vk_indirect_resolve(d,op,&resolved);
        if(rc!=VK_SUCCESS){free(job);return rc;}
        assert(resolved.type==PS5VK_DISPATCH);
        for(unsigned i=0;i<3;++i)state->resolved[i]=resolved.groups[i];
    } else {
        for(uint32_t i=0;i<count;++i)
            assert(command->operations[first+i].type==PS5VK_BARRIER);
        job->producer=VK_TRUE;
    }
    *out=job;return VK_SUCCESS;
}
static VkResult launch(VkDevice d,void *opaque)
{ (void)d;(void)opaque;++((struct job *)opaque)->state->launches;return VK_SUCCESS; }
static VkResult poll(VkDevice d,void *opaque,uint64_t *completed)
{
    (void)d;struct job *job=opaque;++job->state->polls;
    if(job->producer)*job->state->arguments=(VkDispatchIndirectCommand){4,5,6};
    *completed=job->serial;return VK_SUCCESS;
}
static void backend_release(VkDevice d,void *opaque)
{ (void)d;++((struct job *)opaque)->state->releases;free(opaque); }
int main(void)
{
    struct state state={0};struct VkPhysicalDevice_T physical={0};
    for(unsigned i=0;i<3;++i)physical.platform.properties.limits.maxComputeWorkGroupCount[i]=65535;
    struct VkDevice_T d={.physical=&physical,
        .memory={&state,allocate,release,flush,invalidate},
        .buffer_alignment=4,.noncoherent_atom=4,.max_allocation=256,
        .submit_backend={prepare,launch,poll,backend_release}};
    d.queue.device=&d;d.queue.next_serial=1;
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=64};
    VkDeviceMemory memory=NULL;assert(vkAllocateMemory(&d,&mi,NULL,&memory)==VK_SUCCESS);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64,
        .usage=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer=NULL;assert(vkCreateBuffer(&d,&bi,NULL,&buffer)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,buffer,memory,0)==VK_SUCCESS);
    void *mapped=NULL;assert(vkMapMemory(&d,memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    state.arguments=(void *)((unsigned char *)mapped+4);
    *state.arguments=(VkDispatchIndirectCommand){1,1,1};

    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool=NULL;assert(vkCreateCommandPool(&d,&pi,NULL,&pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command=NULL;assert(vkAllocateCommandBuffers(&d,&ai,&command)==VK_SUCCESS);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    VkBufferMemoryBarrier dependency={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=buffer,.offset=0,.size=64};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,0,NULL,1,&dependency,0,NULL);
    struct VkPipeline_T pipeline={.device=&d};
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,&pipeline);
    vkCmdDispatchIndirect(command,buffer,4);
    assert(vkEndCommandBuffer(command)==VK_SUCCESS && command->operation_count==3);
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=1,.pCommandBuffers=&command};
    assert(vkQueueSubmit(&d.queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(state.prepares==1 && state.launches==1 && !state.invalidates);
    assert(d.submission && d.submission->next && d.submission->next->deferred_prepare &&
        !d.submission->next->backend_job);
    /* Every segment keeps the whole command buffer pending.  The generic
     * command-resource invalidator must therefore reject destruction of the
     * deferred indirect argument buffer until the last segment retires. */
    vkDestroyBuffer(&d,buffer,NULL);
    assert(d.lifetime_errors==1 &&
        ps5vk_buffer_usage(&d,buffer,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT));
    assert(ps5vk_queue_poll(&d)==VK_SUCCESS);
    assert(state.prepares==2 && state.launches==2 && state.invalidates==1);
    assert(state.resolved[0]==4 && state.resolved[1]==5 && state.resolved[2]==6);
    assert(ps5vk_queue_poll(&d)==VK_SUCCESS && !d.submission);
    assert(state.releases==2 && command->state==PS5VK_EXECUTABLE);
    d.lifetime_errors=0;

    assert(vkResetCommandBuffer(command,0)==VK_SUCCESS);
    vkDestroyCommandPool(&d,pool,NULL);vkUnmapMemory(&d,memory);
    vkDestroyBuffer(&d,buffer,NULL);vkFreeMemory(&d,memory,NULL);
    assert(!d.lifetime_errors);
    puts("Indirect queue-head deferred prepare: pass (host backend)");
    return 0;
}

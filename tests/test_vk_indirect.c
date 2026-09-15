#include "vk_indirect.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkResult allocate(void *context,VkDeviceSize bytes,void **address,void **backing)
{ (void)context;*address=calloc(1,(size_t)bytes);*backing=*address;return *address?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release(void *context,void *backing)
{ (void)context;free(backing); }
static VkResult cache(void *context,void *backing,VkDeviceSize offset,VkDeviceSize size)
{ (void)context;(void)backing;(void)offset;(void)size;return VK_SUCCESS; }
static VkBuffer buffer(VkDevice d,VkDeviceMemory memory,VkBufferUsageFlags usage,
    VkDeviceSize size,VkDeviceSize offset)
{
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
        .usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer result=NULL;assert(vkCreateBuffer(d,&info,NULL,&result)==VK_SUCCESS);
    assert(vkBindBufferMemory(d,result,memory,offset)==VK_SUCCESS);return result;
}
static VkCommandBuffer command(VkDevice d,VkCommandPool *pool)
{
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(d,&pi,NULL,pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=*pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer result=NULL;assert(vkAllocateCommandBuffers(d,&ai,&result)==VK_SUCCESS);
    return result;
}
static void begin(VkCommandBuffer command)
{
    VkCommandBufferBeginInfo info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command,&info)==VK_SUCCESS);
}
int main(void)
{
    struct VkPhysicalDevice_T physical={0};
    physical.platform.properties.limits.maxComputeWorkGroupCount[0]=65535;
    physical.platform.properties.limits.maxComputeWorkGroupCount[1]=65535;
    physical.platform.properties.limits.maxComputeWorkGroupCount[2]=65535;
    physical.platform.properties.limits.maxDrawIndirectCount=1;
    struct VkDevice_T d={.physical=&physical,.memory={NULL,allocate,release,cache,cache},
        .buffer_alignment=256,.noncoherent_atom=64,.max_allocation=4096};
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=2048};
    VkDeviceMemory memory=NULL;assert(vkAllocateMemory(&d,&mi,NULL,&memory)==VK_SUCCESS);
    VkBuffer indirect=buffer(&d,memory,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,256,0);
    VkBuffer indices=buffer(&d,memory,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,256,256);
    void *mapped=NULL;assert(vkMapMemory(&d,memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);

    VkCommandPool pool=NULL;VkCommandBuffer cb=command(&d,&pool);
    struct VkPipeline_T compute={.device=&d};
    begin(cb);vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,&compute);
    VkDispatchIndirectCommand *dispatch=(void *)((unsigned char *)mapped+4);
    *dispatch=(VkDispatchIndirectCommand){1,2,3};
    vkCmdDispatchIndirect(cb,indirect,4);
    assert(cb->state==PS5VK_RECORDING && cb->operation_count==1);
    assert(cb->operations[0].type==PS5VK_DISPATCH_INDIRECT &&
        cb->operations[0].indirect_buffer==indirect && cb->operations[0].indirect_offset==4);
    *dispatch=(VkDispatchIndirectCommand){7,8,9};
    struct ps5vk_operation resolved;
    assert(ps5vk_indirect_resolve(&d,&cb->operations[0],&resolved)==VK_SUCCESS);
    assert(resolved.type==PS5VK_DISPATCH && resolved.groups[0]==7 &&
        resolved.groups[1]==8 && resolved.groups[2]==9);
    dispatch->x=65536;
    assert(ps5vk_indirect_resolve(&d,&cb->operations[0],&resolved)!=VK_SUCCESS);
    assert(vkResetCommandBuffer(cb,0)==VK_SUCCESS);begin(cb);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,&compute);
    vkCmdDispatchIndirect(cb,indirect,2);
    assert(cb->state==PS5VK_INVALID && !cb->operation_count);

    VkAttachmentDescription pass_attachments[1]={{.format=VK_FORMAT_R8G8B8A8_UNORM}};
    struct ps5vk_subpass pass_subpasses[1]={
        {.color={.attachment=0},.depth={.attachment=VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T pass={.device=&d,.attachment_count=1,.subpass_count=1,
        .attachments=pass_attachments,.subpasses=pass_subpasses};
    struct VkFramebuffer_T framebuffer={.device=&d,.attachment_count=1,
        .color_attachment=0,.depth_attachment=VK_ATTACHMENT_UNUSED};
    struct VkPipeline_T graphics={.device=&d,.graphics=VK_TRUE,
        .graphics_state=(void *)1,.color_format=VK_FORMAT_R8G8B8A8_UNORM,
        .depth_format=VK_FORMAT_UNDEFINED,
        .viewport={.width=16,.height=16,.maxDepth=1},
        .scissor={.extent={16,16}}};
    assert(vkResetCommandBuffer(cb,0)==VK_SUCCESS);begin(cb);
    cb->render_pass=&pass;cb->framebuffer=&framebuffer;cb->graphics_pipeline=&graphics;
    VkDrawIndirectCommand *draw=(void *)((unsigned char *)mapped+32);
    *draw=(VkDrawIndirectCommand){3,1,0,0};
    /* Stride is ignored for one draw and need not satisfy multi-draw rules. */
    vkCmdDrawIndirect(cb,indirect,32,1,3);
    assert(cb->state==PS5VK_RECORDING && cb->operations[0].type==PS5VK_DRAW_INDIRECT);
    assert(ps5vk_indirect_resolve(&d,&cb->operations[0],&resolved)==VK_SUCCESS);
    assert(resolved.type==PS5VK_DRAW && resolved.vertex_count==3 && resolved.instance_count==1);
    draw->firstInstance=1;
    assert(ps5vk_indirect_resolve(&d,&cb->operations[0],&resolved)!=VK_SUCCESS);

    /* With drawCount zero no command bytes are accessed: an aligned offset
     * beyond the buffer is valid, while the buffer must remain live/bound. */
    assert(vkResetCommandBuffer(cb,0)==VK_SUCCESS);begin(cb);
    cb->render_pass=&pass;cb->framebuffer=&framebuffer;cb->graphics_pipeline=&graphics;
    vkCmdDrawIndirect(cb,indirect,4096,0,1);
    assert(cb->state==PS5VK_RECORDING && cb->operations[0].type==PS5VK_DRAW_INDIRECT);
    assert(ps5vk_indirect_resolve(&d,&cb->operations[0],&resolved)==VK_SUCCESS);
    assert(resolved.type==PS5VK_DRAW && !resolved.vertex_count && !resolved.instance_count);

    assert(vkResetCommandBuffer(cb,0)==VK_SUCCESS);begin(cb);
    cb->render_pass=&pass;cb->framebuffer=&framebuffer;cb->graphics_pipeline=&graphics;
    cb->indices=(struct ps5vk_index_binding){indices,0,VK_INDEX_TYPE_UINT32};
    VkDrawIndexedIndirectCommand *indexed=(void *)((unsigned char *)mapped+64);
    *indexed=(VkDrawIndexedIndirectCommand){3,1,0,0,0};
    vkCmdDrawIndexedIndirect(cb,indirect,64,1,0);
    assert(cb->state==PS5VK_RECORDING &&
        cb->operations[0].type==PS5VK_DRAW_INDEXED_INDIRECT);
    assert(ps5vk_indirect_resolve(&d,&cb->operations[0],&resolved)==VK_SUCCESS);
    assert(resolved.type==PS5VK_DRAW_INDEXED && resolved.index_count==3);

    assert(vkResetCommandBuffer(cb,0)==VK_SUCCESS);begin(cb);
    cb->render_pass=&pass;cb->framebuffer=&framebuffer;cb->graphics_pipeline=&graphics;
    vkCmdDrawIndirect(cb,indirect,32,2,sizeof(VkDrawIndirectCommand));
    assert(cb->state==PS5VK_INVALID && !cb->operation_count);
    assert(d.lifetime_errors==2);d.lifetime_errors=0;

    assert(vkResetCommandBuffer(cb,0)==VK_SUCCESS);
    vkDestroyCommandPool(&d,pool,NULL);
    vkUnmapMemory(&d,memory);vkDestroyBuffer(&d,indices,NULL);
    vkDestroyBuffer(&d,indirect,NULL);vkFreeMemory(&d,memory,NULL);
    assert(!d.lifetime_errors);
    puts("Indirect recording and queue-head resolution: pass (host only)");
    return 0;
}

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
/* Records the exact non-coherent range each queue-head resolution invalidates,
 * so the tests can prove one command's bytes - never a neighbour's - are read. */
struct invalidation { VkDeviceSize offset,size;unsigned count; };
static VkResult record_invalidate(void *context,void *backing,VkDeviceSize offset,VkDeviceSize size)
{
    (void)backing;struct invalidation *r=context;
    r->offset=offset;r->size=size;++r->count;return VK_SUCCESS;
}
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
        .viewport_count=1, .viewport={.width=16,.height=16,.maxDepth=1},
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

    /* The checked span helper decides every count/stride shape once. */
    VkDeviceSize length=~(VkDeviceSize)0;
    assert(ps5vk_indirect_argument_span(PS5VK_DISPATCH_INDIRECT,1,0,&length) && length==12);
    assert(!ps5vk_indirect_argument_span(PS5VK_DISPATCH_INDIRECT,0,0,&length));
    assert(!ps5vk_indirect_argument_span(PS5VK_DISPATCH_INDIRECT,2,12,&length));
    assert(ps5vk_indirect_argument_span(PS5VK_DRAW_INDIRECT,0,3,&length) && length==0);
    assert(ps5vk_indirect_argument_span(PS5VK_DRAW_INDIRECT,1,3,&length) && length==16);
    assert(ps5vk_indirect_argument_span(PS5VK_DRAW_INDEXED_INDIRECT,1,0,&length) && length==20);
    assert(!ps5vk_indirect_argument_span(PS5VK_DRAW_INDIRECT,2,12,&length));
    assert(!ps5vk_indirect_argument_span(PS5VK_DRAW_INDIRECT,2,18,&length));
    assert(!ps5vk_indirect_argument_span(PS5VK_DRAW_INDEXED_INDIRECT,2,16,&length));
    assert(ps5vk_indirect_argument_span(PS5VK_DRAW_INDIRECT,2,16,&length) && length==32);
    assert(ps5vk_indirect_argument_span(PS5VK_DRAW_INDEXED_INDIRECT,3,24,&length) && length==68);
    assert(ps5vk_indirect_argument_span(PS5VK_DRAW_INDIRECT,65535,0xfffffffcu,&length) &&
        length==(VkDeviceSize)65534*0xfffffffcu+16);
    assert(!ps5vk_indirect_argument_span(PS5VK_DRAW,2,16,&length));
    assert(!ps5vk_indirect_argument_span(PS5VK_DRAW_INDIRECT,2,16,NULL));
    assert(ps5vk_indirect_argument_size(PS5VK_DRAW_INDIRECT)==16 &&
        ps5vk_indirect_argument_size(PS5VK_DRAW_INDEXED_INDIRECT)==20 &&
        ps5vk_indirect_argument_size(PS5VK_DISPATCH_INDIRECT)==12 &&
        !ps5vk_indirect_argument_size(PS5VK_DRAW));

    /* A device whose platform supports multi-draw and indirect firstInstance
     * and whose application ENABLED both. The physical limit is the core
     * floor the platform helper commits to. */
    struct VkPhysicalDevice_T multi_physical={0};
    multi_physical.platform.supported_features=PS5VK_FEATURE_MULTI_DRAW_INDIRECT|
        PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE;
    multi_physical.platform.properties.limits.maxDrawIndirectCount=
        ps5vk_platform_max_draw_indirect_count(multi_physical.platform.supported_features);
    assert(multi_physical.platform.properties.limits.maxDrawIndirectCount==65535);
    assert(ps5vk_platform_max_draw_indirect_count(0)==1);
    struct invalidation seen={0};
    struct VkDevice_T m={.physical=&multi_physical,
        .memory={&seen,allocate,release,cache,record_invalidate},
        .buffer_alignment=256,.noncoherent_atom=64,.max_allocation=2u<<20,
        .enabled_features=PS5VK_FEATURE_MULTI_DRAW_INDIRECT|PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE};
    enum { ARGS_BYTES=1048576 };
    VkMemoryAllocateInfo multi_mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=ARGS_BYTES+4096};
    VkDeviceMemory multi_memory=NULL;
    assert(vkAllocateMemory(&m,&multi_mi,NULL,&multi_memory)==VK_SUCCESS);
    VkBuffer args=buffer(&m,multi_memory,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,ARGS_BYTES,0);
    VkBuffer multi_indices=buffer(&m,multi_memory,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,1024,ARGS_BYTES);
    void *multi_mapped=NULL;
    assert(vkMapMemory(&m,multi_memory,0,VK_WHOLE_SIZE,0,&multi_mapped)==VK_SUCCESS);
    unsigned char *base=multi_mapped;
    VkCommandPool multi_pool=NULL;VkCommandBuffer mb=command(&m,&multi_pool);
    VkAttachmentDescription multi_attachments[1]={{.format=VK_FORMAT_R8G8B8A8_UNORM}};
    struct ps5vk_subpass multi_subpasses[1]={
        {.color={.attachment=0},.depth={.attachment=VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T multi_pass={.device=&m,.attachment_count=1,.subpass_count=1,
        .attachments=multi_attachments,.subpasses=multi_subpasses};
    struct VkFramebuffer_T multi_framebuffer={.device=&m,.attachment_count=1,
        .color_attachment=0,.depth_attachment=VK_ATTACHMENT_UNUSED};
    struct VkPipeline_T multi_graphics={.device=&m,.graphics=VK_TRUE,
        .graphics_state=(void *)1,.color_format=VK_FORMAT_R8G8B8A8_UNORM,
        .depth_format=VK_FORMAT_UNDEFINED,
        .viewport_count=1, .viewport={.width=16,.height=16,.maxDepth=1},
        .scissor={.extent={16,16}}};
#define MULTI_BEGIN() do { assert(vkResetCommandBuffer(mb,0)==VK_SUCCESS);begin(mb); \
    mb->render_pass=&multi_pass;mb->framebuffer=&multi_framebuffer; \
    mb->graphics_pipeline=&multi_graphics; } while(0)

    /* Three commands at a 32-byte stride from a non-trivial offset: the middle
     * one draws nothing but still owns DrawIndex 1, and each snapshot carries
     * its own firstInstance. Only the 16 bytes of the command being resolved
     * are invalidated. */
    VkDrawIndirectCommand *cmds=(void *)(base+64);
    cmds[0]=(VkDrawIndirectCommand){3,1,0,2};
    cmds[2]=(VkDrawIndirectCommand){0,1,5,1};
    cmds[4]=(VkDrawIndirectCommand){3,2,7,3};
    MULTI_BEGIN();
    vkCmdDrawIndirect(mb,args,64,3,32);
    assert(mb->state==PS5VK_RECORDING && mb->operation_count==1);
    const struct ps5vk_operation *multi_op=&mb->operations[0];
    assert(multi_op->type==PS5VK_DRAW_INDIRECT && multi_op->indirect_count==3 &&
        multi_op->indirect_stride==32 && multi_op->indirect_offset==64 && !multi_op->draw_index);
    assert(ps5vk_indirect_validate(&m,multi_op)==VK_SUCCESS);
    const uint32_t expected_vertices[3]={3,0,3},expected_instances[3]={1,1,2},
        expected_first_vertex[3]={0,5,7},expected_first_instance[3]={2,1,3};
    for(uint32_t k=0;k<3;++k) {
        seen=(struct invalidation){0};
        assert(ps5vk_indirect_resolve_command(&m,multi_op,k,&resolved)==VK_SUCCESS);
        assert(resolved.type==PS5VK_DRAW && resolved.draw_index==k &&
            resolved.vertex_count==expected_vertices[k] &&
            resolved.instance_count==expected_instances[k] &&
            resolved.first_vertex==expected_first_vertex[k] &&
            resolved.first_instance==expected_first_instance[k] &&
            !resolved.index_count && !resolved.first_index && !resolved.vertex_offset &&
            resolved.indirect_buffer==args && resolved.pipeline==&multi_graphics);
        assert(seen.count==1 && seen.offset==64+32u*k && seen.size==16);
    }
    /* No fourth command exists; the single-snapshot entry point has nothing
     * to say about a multi-command record. */
    assert(ps5vk_indirect_resolve_command(&m,multi_op,3,&resolved)!=VK_SUCCESS);
    assert(ps5vk_indirect_resolve_command(&m,multi_op,UINT32_MAX,&resolved)!=VK_SUCCESS);
    assert(ps5vk_indirect_resolve(&m,multi_op,&resolved)!=VK_SUCCESS);
    /* Arguments may change after recording; the queue head sees the new
     * value, and a firstInstance that is legal only with the feature enabled
     * fails closed the moment the logical device lacks it. */
    cmds[2].firstInstance=9;
    assert(ps5vk_indirect_resolve_command(&m,multi_op,1,&resolved)==VK_SUCCESS &&
        resolved.first_instance==9 && resolved.draw_index==1);
    m.enabled_features=PS5VK_FEATURE_MULTI_DRAW_INDIRECT;
    assert(ps5vk_indirect_resolve_command(&m,multi_op,1,&resolved)!=VK_SUCCESS);
    cmds[2].firstInstance=0;
    assert(ps5vk_indirect_resolve_command(&m,multi_op,1,&resolved)==VK_SUCCESS &&
        !resolved.first_instance);
    /* Without multiDrawIndirect enabled a recorded multi-command record is no
     * longer executable, whatever the physical device offers. */
    m.enabled_features=PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE;
    assert(ps5vk_indirect_validate(&m,multi_op)!=VK_SUCCESS);
    assert(ps5vk_indirect_resolve_command(&m,multi_op,0,&resolved)!=VK_SUCCESS);
    MULTI_BEGIN();
    vkCmdDrawIndirect(mb,args,64,3,32);
    assert(mb->state==PS5VK_INVALID && !mb->operation_count);
    /* A single command records and resolves on that device, firstInstance
     * included, because count one is legal without multiDrawIndirect. */
    MULTI_BEGIN();
    vkCmdDrawIndirect(mb,args,64,1,0);
    assert(mb->state==PS5VK_RECORDING &&
        ps5vk_indirect_resolve(&m,&mb->operations[0],&resolved)==VK_SUCCESS &&
        resolved.first_instance==2 && !resolved.draw_index);
    m.enabled_features=PS5VK_FEATURE_MULTI_DRAW_INDIRECT|PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE;
    m.lifetime_errors=0;

    /* Stride rules apply only above one command, and the whole span must fit
     * the buffer: the last command's final byte decides, with the arithmetic
     * checked rather than wrapped. */
    MULTI_BEGIN();vkCmdDrawIndirect(mb,args,64,3,12);
    assert(mb->state==PS5VK_INVALID);
    MULTI_BEGIN();vkCmdDrawIndirect(mb,args,64,3,18);
    assert(mb->state==PS5VK_INVALID);
    MULTI_BEGIN();vkCmdDrawIndirect(mb,args,ARGS_BYTES-80+4,3,32);
    assert(mb->state==PS5VK_INVALID);
    MULTI_BEGIN();vkCmdDrawIndirect(mb,args,ARGS_BYTES-80,3,32);
    assert(mb->state==PS5VK_RECORDING &&
        ps5vk_indirect_validate(&m,&mb->operations[0])==VK_SUCCESS);
    MULTI_BEGIN();vkCmdDrawIndirect(mb,args,0,65536,16);
    assert(mb->state==PS5VK_INVALID);
    MULTI_BEGIN();vkCmdDrawIndirect(mb,args,32,65535,16);
    assert(mb->state==PS5VK_INVALID);
    /* The core floor itself: 65535 packed commands fill the buffer exactly,
     * and the last one resolves with its own index and bytes. */
    MULTI_BEGIN();vkCmdDrawIndirect(mb,args,0,65535,16);
    assert(mb->state==PS5VK_RECORDING &&
        ps5vk_indirect_validate(&m,&mb->operations[0])==VK_SUCCESS);
    VkDrawIndirectCommand *last=(void *)(base+16u*65534u);
    *last=(VkDrawIndirectCommand){6,4,100,65534};
    seen=(struct invalidation){0};
    assert(ps5vk_indirect_resolve_command(&m,&mb->operations[0],65534,&resolved)==VK_SUCCESS);
    assert(resolved.draw_index==65534 && resolved.vertex_count==6 &&
        resolved.instance_count==4 && resolved.first_vertex==100 &&
        resolved.first_instance==65534 && seen.offset==16u*65534u && seen.size==16);
    assert(ps5vk_indirect_resolve_command(&m,&mb->operations[0],65535,&resolved)!=VK_SUCCESS);
    /* The physical limit is re-read at the head: a record whose count exceeds
     * the platform's honest limit is refused even if it was recorded. */
    multi_physical.platform.properties.limits.maxDrawIndirectCount=1;
    assert(ps5vk_indirect_validate(&m,&mb->operations[0])!=VK_SUCCESS);
    multi_physical.platform.properties.limits.maxDrawIndirectCount=65535;
    m.lifetime_errors=0;

    /* Indexed commands: a 24-byte stride over 20-byte structures, a negative
     * vertexOffset, firstIndex/vertexOffset combinations and a non-zero
     * firstInstance, each snapshot exactly its own command. */
    mb->indices=(struct ps5vk_index_binding){multi_indices,0,VK_INDEX_TYPE_UINT32};
    unsigned char *indexed_base=base+256;
    VkDrawIndexedIndirectCommand first_indexed={3,1,0,-2,4},second_indexed={3,1,3,1,0};
    memcpy(indexed_base,&first_indexed,sizeof(first_indexed));
    memcpy(indexed_base+24,&second_indexed,sizeof(second_indexed));
    MULTI_BEGIN();mb->indices=(struct ps5vk_index_binding){multi_indices,0,VK_INDEX_TYPE_UINT32};
    vkCmdDrawIndexedIndirect(mb,args,256,2,24);
    assert(mb->state==PS5VK_RECORDING && mb->operations[0].type==PS5VK_DRAW_INDEXED_INDIRECT);
    seen=(struct invalidation){0};
    assert(ps5vk_indirect_resolve_command(&m,&mb->operations[0],0,&resolved)==VK_SUCCESS);
    assert(resolved.type==PS5VK_DRAW_INDEXED && !resolved.draw_index &&
        resolved.index_count==3 && resolved.instance_count==1 && !resolved.first_index &&
        resolved.vertex_offset==-2 && resolved.first_instance==4 &&
        !resolved.vertex_count && !resolved.first_vertex &&
        resolved.indices.buffer==multi_indices && seen.offset==256 && seen.size==20);
    assert(ps5vk_indirect_resolve_command(&m,&mb->operations[0],1,&resolved)==VK_SUCCESS);
    assert(resolved.draw_index==1 && resolved.first_index==3 && resolved.vertex_offset==1 &&
        !resolved.first_instance && seen.offset==256+24 && seen.size==20);
    MULTI_BEGIN();mb->indices=(struct ps5vk_index_binding){multi_indices,0,VK_INDEX_TYPE_UINT32};
    vkCmdDrawIndexedIndirect(mb,args,256,2,16);
    assert(mb->state==PS5VK_INVALID);
    /* Count one still ignores the stride for indexed commands. */
    MULTI_BEGIN();mb->indices=(struct ps5vk_index_binding){multi_indices,0,VK_INDEX_TYPE_UINT32};
    vkCmdDrawIndexedIndirect(mb,args,256,1,3);
    assert(mb->state==PS5VK_RECORDING &&
        ps5vk_indirect_resolve(&m,&mb->operations[0],&resolved)==VK_SUCCESS &&
        resolved.vertex_offset==-2 && resolved.first_instance==4 && !resolved.draw_index);
    m.lifetime_errors=0;
#undef MULTI_BEGIN

    assert(vkResetCommandBuffer(mb,0)==VK_SUCCESS);
    vkDestroyCommandPool(&m,multi_pool,NULL);
    vkUnmapMemory(&m,multi_memory);vkDestroyBuffer(&m,multi_indices,NULL);
    vkDestroyBuffer(&m,args,NULL);vkFreeMemory(&m,multi_memory,NULL);
    assert(!m.lifetime_errors);
    puts("Indirect recording and queue-head resolution: pass (host only)");
    return 0;
}

#include "vk_command.h"
#include "vk_framebuffer.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static VkResult allocate(void *ctx, VkDeviceSize n, void **address, void **backing)
{ (void)ctx; *address = calloc(1, n); *backing = *address; return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkCommandPool pool(VkDevice d, VkCommandPoolCreateFlags flags)
{
    VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = flags};
    VkCommandPool p; assert(vkCreateCommandPool(d, &info, NULL, &p) == VK_SUCCESS); return p;
}
static VkCommandBuffer command(VkDevice d, VkCommandPool p)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer c; assert(vkAllocateCommandBuffers(d, &info, &c) == VK_SUCCESS); return c;
}
static VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
static void operation_reservation_contract(void)
{
    struct VkDevice_T d={0};
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c=command(&d,p);
    assert(!ps5vk_command_reserve_operations(NULL,PS5VK_DISPATCH,
        PS5VK_OPERATION_ANYWHERE,1));
    assert(!ps5vk_command_reserve_operations(c,PS5VK_DISPATCH,
        PS5VK_OPERATION_ANYWHERE,1));
    assert(c->state==PS5VK_INVALID && !c->operation_count && d.lifetime_errors==1);

    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    memset(&c->operations[0],0xa5,2*sizeof(c->operations[0]));
    struct ps5vk_operation *ops=ps5vk_command_reserve_operations(c,PS5VK_BARRIER,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,2);
    assert(ops==&c->operations[0] && c->operation_count==2);
    assert(ops[0].type==PS5VK_BARRIER && ops[1].type==PS5VK_BARRIER);
    assert(!ops[0].event && !ops[1].pipeline && !ops[1].groups[0]);

    uint32_t source[]={0x11223344u,0xaabbccddu};
    struct ps5vk_operation *owned=ps5vk_command_reserve_operation_with_payload(c,
        PS5VK_DISPATCH,PS5VK_OPERATION_OUTSIDE_RENDER_PASS,source,sizeof(source));
    assert(owned && owned->owned_payload && owned->owned_payload_size==sizeof(source));
    source[0]=0;
    assert(((const uint32_t *)owned->owned_payload)[0]==0x11223344u);

    struct ps5vk_subpass scope_subpasses[1]={{.color={.attachment=0},
        .depth={.attachment=VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T pass={.device=&d,.subpass_count=1,.subpasses=scope_subpasses};
    c->render_pass=&pass;
    unsigned before=c->operation_count;
    assert(!ps5vk_command_reserve_operations(c,PS5VK_DISPATCH,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1));
    assert(c->state==PS5VK_INVALID && c->operation_count==before);

    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    assert(!c->operations[0].owned_payload && !c->operations[0].owned_payload_size);
    assert(!ps5vk_command_reserve_operations(c,PS5VK_DRAW,
        PS5VK_OPERATION_INSIDE_RENDER_PASS,1));
    assert(c->state==PS5VK_INVALID && !c->operation_count);

    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    c->operation_count=PS5VK_MAX_OPERATIONS-1;
    assert(!ps5vk_command_reserve_operations(c,PS5VK_BARRIER,
        PS5VK_OPERATION_ANYWHERE,2));
    assert(c->state==PS5VK_INVALID && c->operation_count==PS5VK_MAX_OPERATIONS-1);

    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    c->state=PS5VK_PENDING;
    assert(!ps5vk_command_reserve_operations(c,PS5VK_BARRIER,
        PS5VK_OPERATION_ANYWHERE,1));
    assert(c->state==PS5VK_PENDING && !c->operation_count);
    c->state=PS5VK_EXECUTABLE;
    vkDestroyCommandPool(&d,p,NULL);
}
static void states(void)
{
    struct VkDevice_T d = {0}; VkCommandPool p = pool(&d, 0); VkCommandBuffer c = command(&d, p);
    assert(c->state == PS5VK_INITIAL && vkEndCommandBuffer(c) != VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) != VK_SUCCESS);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS && c->state == PS5VK_EXECUTABLE);
    assert(vkBeginCommandBuffer(c, &begin_info) != VK_SUCCESS);
    assert(vkResetCommandBuffer(c, 0) != VK_SUCCESS);
    c->state = PS5VK_PENDING; /* State fixture only; no submit occurred. */
    assert(vkResetCommandPool(&d, p, 0) != VK_SUCCESS);
    vkFreeCommandBuffers(&d, p, 1, &c); assert(p->buffers == c);
    vkDestroyCommandPool(&d, p, NULL); assert(d.command_pools == p);
    c->state = PS5VK_EXECUTABLE;
    assert(vkResetCommandPool(&d, p, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) == VK_SUCCESS);
    assert(c->state == PS5VK_INITIAL); vkDestroyCommandPool(&d, p, NULL); assert(!d.command_pools);
    p = pool(&d, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT); c = command(&d, p);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS && vkEndCommandBuffer(c) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    for (unsigned j = 0; j < PS5VK_MAX_OPERATIONS; ++j)
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 0, NULL);
    assert(c->operation_count == PS5VK_MAX_OPERATIONS && c->state == PS5VK_RECORDING);
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 0, NULL);
    assert(c->state == PS5VK_INVALID && vkEndCommandBuffer(c) != VK_SUCCESS);
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS && !c->operation_count);
    vkDestroyCommandPool(&d, p, NULL); assert(!d.command_pools);
}
static void stage_access_scopes(void)
{
    struct VkDevice_T d={0};
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c=command(&d,p);
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,NULL,0,NULL);
    assert(c->state==PS5VK_RECORDING && c->operation_count==1);

    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,NULL,0,NULL);
    assert(c->state==PS5VK_INVALID && !c->operation_count);

    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,NULL,0,NULL);
    assert(c->state==PS5VK_RECORDING && c->operation_count==1);

    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,NULL,0,NULL);
    assert(c->state==PS5VK_INVALID && !c->operation_count);

    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,NULL,0,NULL);
    assert(c->state==PS5VK_RECORDING && c->operation_count==1);

    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS &&
        vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,NULL,0,NULL);
    assert(c->state==PS5VK_RECORDING && c->operation_count==1);
    vkDestroyCommandPool(&d,p,NULL);
}
static void recording_and_invalidation(void)
{
    struct VkDevice_T d = {.memory = {NULL, allocate, release, cache, cache},
        .buffer_alignment = 256, .noncoherent_atom = 64, .max_allocation = 4096};
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding}; VkDescriptorSetLayout set_layout;
    assert(vkCreateDescriptorSetLayout(&d, &si, NULL, &set_layout) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout}; VkPipelineLayout layout;
    assert(vkCreatePipelineLayout(&d, &li, NULL, &layout) == VK_SUCCESS);
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size}; VkDescriptorPool descriptors;
    assert(vkCreateDescriptorPool(&d, &pi, NULL, &descriptors) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &set_layout}; VkDescriptorSet set;
    assert(vkAllocateDescriptorSets(&d, &ai, &set) == VK_SUCCESS);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 512, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}; VkBuffer buffer;
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 1024}; VkDeviceMemory memory;
    assert(vkCreateBuffer(&d, &bi, NULL, &buffer) == VK_SUCCESS);
    assert(vkAllocateMemory(&d, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, buffer, memory, 256) == VK_SUCCESS);
    VkDescriptorBufferInfo value = {buffer, 256, 256};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &value};
    vkUpdateDescriptorSets(&d, 1, &write, 0, NULL);
    /* Internal recording fixture: no ISA and never submitted. */
    struct VkPipeline_T pipeline = {.device = &d, .set_count = 1,
        .program = {.descriptor_set_mask=1,.descriptor_count = 1,
            .descriptors = {{0, 0, 0, 0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}}};
    pipeline.sets[0] = set_layout->signature;
    VkCommandPool p = pool(&d, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT); VkCommandBuffer c = command(&d, p);
    VkBufferMemoryBarrier bb = {.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=buffer, .offset=0, .size=VK_WHOLE_SIZE};
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,NULL,1,&bb,0,NULL);
    assert(vkEndCommandBuffer(c)==VK_SUCCESS && c->operations[0].buffer_barrier.buffer==buffer);
    /* Fragment stores use the same core shader-write access bit as compute.
     * The focused CTS records this exact SSBO-to-host dependency after its
     * render pass; accepting compute but rejecting fragment made the valid
     * void command poison the buffer and surface only at EndCommandBuffer. */
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,NULL,1,&bb,0,NULL);
    assert(c->state==PS5VK_RECORDING && c->operation_count==2);
    assert(c->operations[0].src_stage==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    assert(c->operations[0].src_access==VK_ACCESS_SHADER_WRITE_BIT);
    assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    c->state=PS5VK_PENDING;
    vkDestroyBuffer(&d,buffer,NULL); assert(d.buffers==buffer);
    c->state=PS5VK_EXECUTABLE;
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    bb.offset=513;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,NULL,1,&bb,0,NULL);
    assert(c->state==PS5VK_INVALID && c->operation_count==0);
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    bb.offset=0; bb.dstQueueFamilyIndex=1;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,NULL,1,&bb,0,NULL);
    assert(c->state==PS5VK_INVALID && c->operation_count==0);
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, &pipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    vkCmdDispatch(c, 16, 1, 1);
    assert(c->operation_count == 2 && c->operations[0].type == PS5VK_BARRIER);
    assert(c->operations[1].groups[0] == 16 && c->operations[1].generations[0] == set->generation);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    c->state = PS5VK_PENDING; /* Synthetic ownership state only. */
    vkFreeMemory(&d, memory, NULL); assert(d.memories == memory);
    vkDestroyBuffer(&d, buffer, NULL); assert(d.buffers == buffer);
    uint64_t generation = set->generation;
    vkUpdateDescriptorSets(&d, 1, &write, 0, NULL); assert(set->generation == generation);
    assert(vkResetDescriptorPool(&d, descriptors, 0) != VK_SUCCESS);
    c->state = PS5VK_EXECUTABLE;
    vkUpdateDescriptorSets(&d, 1, &write, 0, NULL); assert(c->state == PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, &pipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(c, 1, 1, 1); assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    assert(vkResetDescriptorPool(&d, descriptors, 0) == VK_SUCCESS && c->state == PS5VK_INVALID);
    /* Invalidation after set destruction must not dereference stale records. */
    vkDestroyBuffer(&d, buffer, NULL); vkFreeMemory(&d, memory, NULL);
    vkDestroyDescriptorPool(&d, descriptors, NULL); vkDestroyPipelineLayout(&d, layout, NULL);
    vkDestroyDescriptorSetLayout(&d, set_layout, NULL); vkDestroyCommandPool(&d, p, NULL);
    assert(!d.command_pools && !d.buffers && !d.memories && !d.descriptor_objects);
}
static void multi_set_recording(void)
{
    struct VkDevice_T d={.memory={NULL,allocate,release,cache,cache},
        .buffer_alignment=256,.uniform_buffer_alignment=256,
        .noncoherent_atom=64,.max_allocation=4096};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=256,
        .usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=256};
    VkBuffer uniform;VkDeviceMemory memory;
    assert(vkCreateBuffer(&d,&bi,NULL,&uniform)==VK_SUCCESS);
    assert(vkAllocateMemory(&d,&mi,NULL,&memory)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,uniform,memory,0)==VK_SUCCESS);
    struct VkDescriptorPool_T descriptor_pool={.device=&d};
    struct VkDescriptorSet_T a={.pool=&descriptor_pool,.generation=3,.defined={VK_TRUE}};
    struct VkDescriptorSet_T cset={.pool=&descriptor_pool,.generation=7,.defined={VK_TRUE}};
    a.signature.binding[0]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_COMPUTE_BIT};
    cset.signature.binding[0]=a.signature.binding[0];
    a.signature.type[0]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cset.signature.type[0]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    struct VkPipelineLayout_T layout={.device=&d,.set_count=3};
    layout.sets[0]=a.signature;layout.sets[2]=cset.signature;
    struct VkPipeline_T pipeline={.device=&d,.set_count=3,
        .program={.descriptor_set_mask=5,.descriptor_count=2,
            .descriptors={{0,0,0,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                          {2,0,0,0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}}}};
    memcpy(pipeline.sets,layout.sets,sizeof(pipeline.sets));
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer cmd=command(&d,p);assert(vkBeginCommandBuffer(cmd,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,&pipeline);
    VkDescriptorSet set2=&cset;vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,
        &layout,2,1,&set2,0,NULL);
    VkDescriptorSet set0=&a;vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,
        &layout,0,1,&set0,0,NULL);
    VkBufferMemoryBarrier uniform_ready={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=uniform,.offset=0,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,0,NULL,1,&uniform_ready,0,NULL);
    vkCmdDispatch(cmd,1,1,1);
    assert(cmd->state==PS5VK_RECORDING && cmd->operation_count==3 &&
        cmd->operations[0].dst_access==VK_ACCESS_UNIFORM_READ_BIT &&
        cmd->operations[2].sets[0]==&a && cmd->operations[2].sets[2]==&cset &&
        cmd->operations[2].generations[0]==3 && cmd->operations[2].generations[2]==7);
    assert(vkEndCommandBuffer(cmd)==VK_SUCCESS);
    assert(vkResetCommandBuffer(cmd,0)==VK_SUCCESS && vkBeginCommandBuffer(cmd,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,&pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,&layout,0,1,&set0,0,NULL);
    vkCmdDispatch(cmd,1,1,1);assert(cmd->state==PS5VK_INVALID);
    assert(vkResetCommandBuffer(cmd,0)==VK_SUCCESS && vkBeginCommandBuffer(cmd,&begin_info)==VK_SUCCESS);
    uniform_ready.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,0,NULL,1,&uniform_ready,0,NULL);
    assert(cmd->state==PS5VK_INVALID && !cmd->operation_count);
    vkDestroyCommandPool(&d,p,NULL);vkDestroyBuffer(&d,uniform,NULL);vkFreeMemory(&d,memory,NULL);
}
/* The vkCmdNextSubpass state machine, built through the PUBLIC entry points.
 *
 * The render pass, the image view and the framebuffer are created with
 * vkCreateRenderPass, vkCreateImageView and vkCreateFramebuffer rather than
 * assembled as structs, so what is under test is the driver's own acceptance
 * of the shape and not a fixture that agrees with itself.
 *
 * Nothing here executes: this test covers the object model and recording
 * transitions, while submitting a pass with more than one subpass stays
 * fail-closed until native execution implements it. */
static VkResult image_requirements(VkDevice d, const VkImageCreateInfo *i,
                                   VkMemoryRequirements *r)
{ (void)d; (void)i; *r = (VkMemoryRequirements){4096, 256, 1}; return VK_SUCCESS; }

static void subpass_transitions(void)
{
    struct VkDevice_T d = {.graphics_enabled = VK_TRUE,
        .memory={NULL,allocate,release,cache,cache},.buffer_alignment=256,
        .noncoherent_atom=64,.max_allocation=1<<20,
        .image_requirements=image_requirements};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {8,8,1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImage image; VkDeviceMemory memory;
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 4096};
    assert(vkCreateImage(&d, &ii, NULL, &image) == VK_SUCCESS);
    assert(vkAllocateMemory(&d, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(&d, image, memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1}};
    VkImageView view;
    assert(vkCreateImageView(&d, &vi, NULL, &view) == VK_SUCCESS);

    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference colour = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    /* Both subpasses name the SAME attachment, which is the only shape a
     * framebuffer in this driver can serve. */
    VkSubpassDescription described[2] = {
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &colour},
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &colour}};
    VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 2, .pSubpasses = described};
    VkRenderPass two, one;
    assert(vkCreateRenderPass(&d, &rpi, NULL, &two) == VK_SUCCESS);
    rpi.subpassCount = 1;
    assert(vkCreateRenderPass(&d, &rpi, NULL, &one) == VK_SUCCESS);
    /* A framebuffer created for the two-subpass pass really is usable with it,
     * and the role it stored is the one both subpasses name. */
    VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = two, .attachmentCount = 1, .pAttachments = &view,
        .width = 8, .height = 8, .layers = 1};
    VkFramebuffer fb;
    assert(vkCreateFramebuffer(&d, &fbi, NULL, &fb) == VK_SUCCESS);
    assert(ps5vk_framebuffer_compatible(fb, two) &&
           ps5vk_framebuffer_compatible(fb, one) &&
           !fb->color_attachment && fb->depth_attachment == VK_ATTACHMENT_UNUSED);

    /* One pipeline per subpass. Creation is exercised publicly in
     * tests/test_vk_graphics_pipeline.c; here the identity is what matters. */
    struct VkPipeline_T first = {.device = &d, .graphics = VK_TRUE, .subpass = 0,
        .color_format = VK_FORMAT_B8G8R8A8_UNORM,
        .viewport_count=1, .viewport={0,0,8,8,0,1}, .scissor = {{0,0},{8,8}}};
    struct VkPipeline_T second = first; second.subpass = 1;
    VkClearValue value = {.color = {.float32 = {0, 0, 0, 1}}};
    VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = two, .framebuffer = fb, .renderArea = {.extent = {8, 8}},
        .clearValueCount = 1, .pClearValues = &value};
    VkCommandPool p = pool(&d, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c = command(&d, p);

    /* The accepted shape: draw, advance, draw, end. Each operation says which
     * subpass it belongs to. */
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &first);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    assert(!c->subpass);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_RECORDING && c->subpass == 1);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &second);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRenderPass(c);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS && c->operation_count == 5);
    assert(c->operations[0].type == PS5VK_BEGIN_RENDER_PASS && !c->operations[0].subpass);
    assert(c->operations[1].type == PS5VK_DRAW && !c->operations[1].subpass);
    assert(c->operations[2].type == PS5VK_NEXT_SUBPASS && c->operations[2].subpass == 1);
    assert(c->operations[3].type == PS5VK_DRAW && c->operations[3].subpass == 1);
    assert(c->operations[4].type == PS5VK_END_RENDER_PASS && c->operations[4].subpass == 1);

    /* A PIPELINE BELONGS TO ONE SUBPASS: drawing in subpass 1 with the
     * pipeline created for subpass 0 is refused even though every format
     * agrees, because it was never compiled for that scope. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &first);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdDraw(c, 3, 1, 0, 0);
    assert(c->state == PS5VK_INVALID && c->operation_count == 3);
    /* And the same pipeline in the subpass it does belong to is accepted. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &second);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdDraw(c, 3, 1, 0, 0);
    assert(c->state == PS5VK_INVALID && c->operation_count == 1);

    /* AN EMPTY SUBPASS IS LEGAL TO RECORD. Vulkan permits it - the load and
     * store ops alone are observable - so the transition and the end are
     * recorded faithfully with zero draws, and whether this driver can
     * EXECUTE that is decided at submission, not here. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_RECORDING && c->subpass == 1);
    vkCmdEndRenderPass(c);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS && c->operation_count == 3 &&
           c->operations[1].type == PS5VK_NEXT_SUBPASS);
    /* An entirely empty single-subpass pass records just as legally. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    ri.renderPass = one;
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(c);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS && c->operation_count == 2);
    ri.renderPass = two;

    /* Refused, each transactionally. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_INVALID && !c->operation_count);
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdNextSubpass(c, (VkSubpassContents)7);
    assert(c->state == PS5VK_INVALID && c->operation_count == 1);
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_INVALID && c->operation_count == 2);
    /* Ending before the last subpass would drop the one never entered. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(c);
    assert(c->state == PS5VK_INVALID && c->operation_count == 1);
    /* A single-subpass pass has no next subpass at all. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    ri.renderPass = one;
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_INVALID && c->operation_count == 1);
    ri.renderPass = two;

    /* A CONTINUATION SECONDARY IS RECORDED FOR ONE SUBPASS and may execute in
     * that one only. */
    VkCommandBufferAllocateInfo si = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p, .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = 1};
    VkCommandBuffer secondary;
    assert(vkAllocateCommandBuffers(&d, &si, &secondary) == VK_SUCCESS);
    VkCommandBufferInheritanceInfo inherit = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
        .renderPass = two, .subpass = 1, .framebuffer = fb};
    VkCommandBufferBeginInfo sbegin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
        .pInheritanceInfo = &inherit};
    assert(vkBeginCommandBuffer(secondary, &sbegin) == VK_SUCCESS);
    assert(secondary->subpass == 1 && secondary->render_pass_inherited);
    vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, &second);
    vkCmdDraw(secondary, 3, 1, 0, 0);
    assert(vkEndCommandBuffer(secondary) == VK_SUCCESS);
    /* Named in subpass 1: accepted. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(c, 1, &secondary);
    assert(c->state == PS5VK_RECORDING && c->operation_count == 3 &&
           c->operations[2].subpass == 1);
    /* Named in subpass 0: refused, because it was recorded for a different
     * scope even though the pass is the same object. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(c, 1, &secondary);
    assert(c->state == PS5VK_INVALID && c->operation_count == 1);
    /* A secondary never advances a subpass: it inherits one. */
    assert(vkResetCommandBuffer(secondary, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(secondary, &sbegin) == VK_SUCCESS);
    vkCmdNextSubpass(secondary, VK_SUBPASS_CONTENTS_INLINE);
    assert(secondary->state == PS5VK_INVALID && !secondary->operation_count);
    /* A subpass the inherited pass does not have is refused at begin. */
    assert(vkResetCommandBuffer(secondary, 0) == VK_SUCCESS);
    inherit.subpass = 2;
    assert(vkBeginCommandBuffer(secondary, &sbegin) != VK_SUCCESS);

    vkFreeCommandBuffers(&d, p, 1, &secondary);
    vkDestroyCommandPool(&d, p, NULL);
    fb->pending = 0;
    vkDestroyFramebuffer(&d, fb, NULL);
    vkDestroyImageView(&d, view, NULL);
    vkDestroyRenderPass(&d, one, NULL);
    vkDestroyRenderPass(&d, two, NULL);
    vkDestroyImage(&d, image, NULL);
    vkFreeMemory(&d, memory, NULL);
}

static void graphics_recording(void)
{
    /* Structural objects only: no shaders, allocation or GPU execution. */
    struct VkDevice_T d = {.graphics_enabled = VK_TRUE,
        .memory={NULL,allocate,release,cache,cache},.buffer_alignment=256,
        .noncoherent_atom=64,.max_allocation=4096};
    struct VkImage_T image = {.device = &d};
    struct VkImageView_T view = {.device = &d, .image = &image};
    VkAttachmentDescription pass_attachments[1] = {
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR}};
    struct ps5vk_subpass pass_subpasses[1] = {
        {.color = {.attachment = 0}, .depth = {.attachment = VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T pass = {.device = &d, .attachment_count = 1, .subpass_count = 1,
        .attachments = pass_attachments, .subpasses = pass_subpasses};
    struct VkFramebuffer_T fb = {.device = &d, .width = 100, .height = 100, .attachment_count = 1,
        .attachments = {&view}, .formats = {VK_FORMAT_B8G8R8A8_UNORM}, .samples = {VK_SAMPLE_COUNT_1_BIT},
        .depth_attachment = VK_ATTACHMENT_UNUSED};
    struct VkPipeline_T pipeline = {.device = &d, .graphics = VK_TRUE,
        .color_format = VK_FORMAT_B8G8R8A8_UNORM,
        .viewport_count=1, .viewport={0,0,100,100,0,1}, .scissor = {{0,0},{100,100}}};
    VkClearValue value = {.color = {.float32 = {0.25f, 0, 0, 1}}};
    VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = &pass, .framebuffer = &fb, .renderArea = {.extent = {100, 100}},
        .clearValueCount = 1, .pClearValues = &value};
    VkCommandPool p = pool(&d, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c = command(&d, p);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &pipeline);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    value.color.float32[0] = 1;
    assert(c->operations[0].clears[0].color.float32[0] == 0.25f);
    assert(vkEndCommandBuffer(c) != VK_SUCCESS);
    struct VkDescriptorPool_T graphics_pool={.device=&d};
    struct VkDescriptorSet_T graphics_set={.pool=&graphics_pool,.generation=9};
    graphics_set.signature.count=1;
    graphics_set.signature.binding[0]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_FRAGMENT_BIT};
    graphics_set.signature.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    graphics_set.defined[0]=VK_TRUE;graphics_set.images[0].imageView=&view;
    graphics_set.image_resources[0]=&image;
    struct VkPipelineLayout_T graphics_layout={.device=&d,.set_count=1};
    graphics_layout.sets[0]=graphics_set.signature;
    VkDescriptorSet graphics_handle=&graphics_set;
    vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&graphics_layout,0,1,&graphics_handle,0,NULL);
    assert(c->graphics_sets[0]==graphics_handle && !c->sets[0]);
    pipeline.set_count=1;pipeline.sets[0]=graphics_set.signature;
    struct VkDescriptorSet_T extra_sets[3];VkDescriptorSet extra_handles[3];
    pipeline.set_count=graphics_layout.set_count=4;
    for(unsigned s=1;s<4;++s) {
        extra_sets[s-1]=graphics_set;extra_sets[s-1].generation=9+s;
        extra_handles[s-1]=&extra_sets[s-1];
        pipeline.sets[s]=graphics_layout.sets[s]=graphics_set.signature;
    }
    vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&graphics_layout,3,1,extra_handles+2,0,NULL);
    vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&graphics_layout,1,2,extra_handles,0,NULL);
    /* Exercise the runtime-table snapshot independently of the precompiled
     * descriptor list (empty in runtime graphics pipelines). */
    assert(pipeline.program.descriptor_count==0);
    for(unsigned s=0;s<4;++s)c->graphics_set_dynamic_offsets[s][0]=256u*(s+1u);
    vkCmdDraw(c, 3, 1, 2, 4);
    for(unsigned s=0;s<4;++s) {
        c->graphics_set_dynamic_offsets[s][0]=0;
        assert(c->operations[1].graphics_dynamic_offsets[s][0]==256u*(s+1u));
    }
    assert(c->operations[1].sets[0]==graphics_handle && c->operations[1].generations[0]==9);
    for(unsigned s=1;s<4;++s)assert(c->operations[1].sets[s]==extra_handles[s-1] &&
        c->operations[1].generations[s]==9+s);
    vkCmdEndRenderPass(c);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS && c->operation_count == 3);
    assert(c->operations[1].first_vertex == 2 && c->operations[1].first_instance == 4);
    assert(c->operations[1].viewport.width==100 && c->operations[1].scissor.extent.width==100);
    c->state = PS5VK_PENDING;
    assert(!d.invalidate(&d, VK_OBJECT_TYPE_IMAGE, &image));
    assert(!d.invalidate(&d, VK_OBJECT_TYPE_FRAMEBUFFER, &fb));
    assert(!d.invalidate(&d, VK_OBJECT_TYPE_PIPELINE, &pipeline));
    assert(!d.invalidate(&d, VK_OBJECT_TYPE_DESCRIPTOR_SET, graphics_handle));
    for(unsigned s=0;s<3;++s)assert(!d.invalidate(&d,VK_OBJECT_TYPE_DESCRIPTOR_SET,extra_handles[s]));
    c->state = PS5VK_EXECUTABLE;
    assert(d.invalidate(&d, VK_OBJECT_TYPE_IMAGE_VIEW, &view) && c->state == PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS && !c->graphics_pipeline && !c->graphics_sets[0]);
    pipeline.set_count=0;
    {
        struct VkPipeline_T unused=pipeline;
        unused.set_count=1;unused.sets[0]=graphics_set.signature;
        for(unsigned mode=0;mode<3;++mode) {
            unused.graphics_usage_known=mode!=0;
            unused.graphics_used_set_mask=mode==1?1u:0u;
            if(mode)assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&unused);
            vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdDraw(c,3,1,0,0);
            if(mode<2)assert(c->state==PS5VK_INVALID);
            else {
                assert(c->state==PS5VK_RECORDING && !c->operations[1].sets[0]);
                vkCmdEndRenderPass(c);assert(vkEndCommandBuffer(c)==VK_SUCCESS);
            }
        }
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    }
    ri.clearValueCount = 0;
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_INVALID && !c->operation_count);
    ri.clearValueCount = 1;
    struct VkPipeline_T dynamic_pipeline=pipeline;
    dynamic_pipeline.dynamic_viewport=dynamic_pipeline.dynamic_scissor=VK_TRUE;
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&dynamic_pipeline);
    vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdDraw(c,3,1,0,0);
    assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&dynamic_pipeline);
    vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport dynamic_viewport={10,20,40,50,0.25f,0.75f};
    VkRect2D dynamic_scissor={{12,22},{30,32}};
    vkCmdSetViewport(c,0,1,&dynamic_viewport);
    vkCmdSetScissor(c,0,1,&dynamic_scissor);
    vkCmdDraw(c,3,1,0,0);
    assert(c->state==PS5VK_RECORDING && c->operation_count==2);
    assert(c->operations[1].viewport.x==10 && c->operations[1].viewport.width==40 &&
        c->operations[1].scissor.offset.x==12 && c->operations[1].scissor.extent.height==32);
    dynamic_viewport.x=99;dynamic_scissor.offset.x=99;
    assert(c->operations[1].viewport.x==10 && c->operations[1].scissor.offset.x==12);
    /* A static-bias pipeline's draw carries the pipeline's snapshot by value. */
    assert(!c->operations[1].raster.depth_bias_enable &&
        c->operations[1].raster.depth_bias_constant==0.0f);
    vkCmdEndRenderPass(c);assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    {
        /* Viewport/scissor arrays. Without multiViewport enabled the setters
         * take exactly (0,1). With it, any first/count inside the 16-entry
         * array is stored index by index, partial updates keep the other
         * indices, a draw needs every index below its pipeline's count, and
         * each draw snapshots the arrays by value. */
        VkViewport vps[PS5VK_MAX_VIEWPORTS]; VkRect2D scs[PS5VK_MAX_VIEWPORTS];
        for(unsigned i=0;i<PS5VK_MAX_VIEWPORTS;++i) {
            vps[i]=(VkViewport){(float)(10*i),0,10,10,0,1};
            scs[i]=(VkRect2D){{(int32_t)(10*i),0},{10,10}};
        }
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdSetViewport(c,0,2,vps);assert(c->state==PS5VK_INVALID);
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdSetScissor(c,1,1,scs);assert(c->state==PS5VK_INVALID);
        d.enabled_features|=PS5VK_FEATURE_MULTI_VIEWPORT;
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdSetViewport(c,0,0,vps);assert(c->state==PS5VK_INVALID);
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdSetViewport(c,15,2,vps);assert(c->state==PS5VK_INVALID);
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdSetScissor(c,16,1,scs);assert(c->state==PS5VK_INVALID);
        /* One bad element rejects the call and leaves the array untouched. */
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdSetViewport(c,0,2,vps);
        VkViewport bad[2]={{0,0,5,5,0,1},{0,0,NAN,5,0,1}};
        vkCmdSetViewport(c,0,2,bad);
        assert(c->state==PS5VK_INVALID && c->viewports[0].width==10 && c->viewports[1].width==10);
        /* Partial updates: indices 2..3 then 0..1, the mask tracks every set index. */
        struct VkPipeline_T quad=pipeline;
        quad.dynamic_viewport=quad.dynamic_scissor=VK_TRUE;quad.viewport_count=4;
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&quad);
        vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(c,2,2,vps+2);vkCmdSetScissor(c,0,4,scs);
        assert(c->viewport_valid==0xcu && c->scissor_valid==0xfu);
        vkCmdDraw(c,3,1,0,0);assert(c->state==PS5VK_INVALID); /* viewports 0..1 unset */
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&quad);
        vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(c,2,2,vps+2);vkCmdSetViewport(c,0,2,vps);vkCmdSetScissor(c,0,4,scs);
        vkCmdDraw(c,3,1,0,0);
        assert(c->state==PS5VK_RECORDING && c->operations[1].viewport_count==4);
        assert(c->operations[1].viewports[3].x==30 && c->operations[1].scissors[3].offset.x==30 &&
            c->operations[1].viewport.x==0);
        /* A later partial update reaches only later draws, index by index. */
        VkViewport moved={99,0,10,10,0,1};VkRect2D moved_scissor={{99,0},{10,10}};
        vkCmdSetViewport(c,1,1,&moved);vkCmdSetScissor(c,3,1,&moved_scissor);
        vkCmdDraw(c,3,1,0,0);
        assert(c->operations[1].viewports[1].x==10 && c->operations[1].scissors[3].offset.x==30);
        assert(c->operations[2].viewports[1].x==99 && c->operations[2].viewports[0].x==0 &&
            c->operations[2].viewports[2].x==20 && c->operations[2].scissors[3].offset.x==99 &&
            c->operations[2].scissors[2].offset.x==20);
        /* A single-viewport dynamic pipeline needs only index 0 and copies one. */
        struct VkPipeline_T single=pipeline;
        single.dynamic_viewport=single.dynamic_scissor=VK_TRUE;
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&single);
        vkCmdDraw(c,3,1,0,0);
        assert(c->state==PS5VK_RECORDING && c->operations[3].viewport_count==1 &&
            c->operations[3].viewport.x==0 && c->operations[3].viewports[1].x==0);
        /* A static array pipeline snapshots its own arrays, not the buffer's. */
        struct VkPipeline_T static_quad=pipeline;
        static_quad.viewport_count=3;
        for(unsigned i=0;i<3;++i){static_quad.viewports[i]=(VkViewport){(float)(100+i),0,4,4,0,1};
            static_quad.scissors[i]=(VkRect2D){{(int32_t)(100+i),0},{4,4}};}
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&static_quad);
        vkCmdDraw(c,3,1,0,0);
        assert(c->state==PS5VK_RECORDING && c->operations[4].viewport_count==3 &&
            c->operations[4].viewports[2].x==102 && c->operations[4].scissors[2].offset.x==102);
        vkCmdEndRenderPass(c);assert(vkEndCommandBuffer(c)==VK_SUCCESS);
        /* Reset clears every index. */
        assert(vkResetCommandBuffer(c,0)==VK_SUCCESS && !c->viewport_valid && !c->scissor_valid);
        d.enabled_features&=~PS5VK_FEATURE_MULTI_VIEWPORT;
        /* The dynamic (0,1) path still works without the feature. */
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdSetViewport(c,0,1,vps);vkCmdSetScissor(c,0,1,scs);
        assert(c->state==PS5VK_RECORDING && c->viewport_valid==1u && c->scissor_valid==1u);
        assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    }
    {
        /* Dynamic depth bias. The factors are required only when the bias is
         * enabled; each draw snapshots the CURRENT factors, a later setter
         * does not reach an earlier draw, and a static-bias pipeline bound in
         * between keeps its own values. Reset drops the dynamic state. */
        struct VkPipeline_T bias_pipeline=pipeline;
        bias_pipeline.dynamic_depth_bias=VK_TRUE;
        bias_pipeline.raster.depth_bias_enable=VK_TRUE;
        struct VkPipeline_T static_pipeline=pipeline;
        static_pipeline.raster=(struct ps5vk_raster_state){.depth_bias_enable=VK_TRUE,
            .depth_bias_constant=4.0f,.depth_bias_slope=0.5f,.depth_clamp=VK_TRUE};
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&bias_pipeline);
        vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdDraw(c,3,1,0,0);
        assert(c->state==PS5VK_INVALID); /* enabled dynamic bias never set */
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&bias_pipeline);
        vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetDepthBias(c,-1.5f,0.0f,2.25f);
        vkCmdDraw(c,3,1,0,0);
        vkCmdSetDepthBias(c,7.0f,0.0f,-3.0f);
        vkCmdDraw(c,3,1,0,0);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&static_pipeline);
        vkCmdDraw(c,3,1,0,0);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&bias_pipeline);
        vkCmdDraw(c,3,1,0,0);
        assert(c->state==PS5VK_RECORDING && c->operation_count==5);
        const struct ps5vk_raster_state *r1=&c->operations[1].raster,*r2=&c->operations[2].raster,
            *r3=&c->operations[3].raster,*r4=&c->operations[4].raster;
        assert(r1->depth_bias_enable && r1->depth_bias_constant==-1.5f && r1->depth_bias_slope==2.25f);
        assert(r2->depth_bias_enable && r2->depth_bias_constant==7.0f && r2->depth_bias_slope==-3.0f);
        assert(r3->depth_bias_enable && r3->depth_bias_constant==4.0f && r3->depth_bias_slope==0.5f);
        assert(r3->depth_clamp && !r2->depth_clamp && !r4->depth_clamp);
        assert(r4->depth_bias_enable && r4->depth_bias_constant==7.0f && r4->depth_bias_slope==-3.0f);
        vkCmdEndRenderPass(c);assert(vkEndCommandBuffer(c)==VK_SUCCESS);
        /* A dynamic-bias pipeline with the bias DISABLED needs no setter: the
         * factors are ignored, so the draw records with them at zero. */
        bias_pipeline.raster.depth_bias_enable=VK_FALSE;
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&bias_pipeline);
        vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdDraw(c,3,1,0,0);
        assert(c->state==PS5VK_RECORDING && !c->operations[1].raster.depth_bias_enable &&
            c->operations[1].raster.depth_bias_constant==0.0f);
        vkCmdEndRenderPass(c);assert(vkEndCommandBuffer(c)==VK_SUCCESS);
        /* Reset clears the dynamic factors: the next enabled draw needs them again. */
        bias_pipeline.raster.depth_bias_enable=VK_TRUE;
        assert(vkResetCommandBuffer(c,0)==VK_SUCCESS && !(c->dynamic_state_valid&2u));
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&bias_pipeline);
        vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdDraw(c,3,1,0,0);
        assert(c->state==PS5VK_INVALID);
    }
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    dynamic_viewport=(VkViewport){0,0,NAN,1,0,1};
    vkCmdSetViewport(c,0,1,&dynamic_viewport);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetScissor(c,1,1,&dynamic_scissor);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetViewport(c,0,1,NULL);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    vkCmdDraw(c, 3, 1, 0, 0);
    assert(c->state == PS5VK_INVALID);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=256,
        .usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer indices[2];
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=512};
    VkDeviceMemory memory;assert(vkAllocateMemory(&d,&mi,NULL,&memory)==VK_SUCCESS);
    for(unsigned i=0;i<2;++i) {
        assert(vkCreateBuffer(&d,&bi,NULL,&indices[i])==VK_SUCCESS);
        assert(vkBindBufferMemory(&d,indices[i],memory,256*i)==VK_SUCCESS);
    }
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&pipeline);
    vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindIndexBuffer(c,indices[0],4,VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(c,6,2,3,-2,7);
    assert(c->state==PS5VK_RECORDING && c->operation_count==2);
    const struct ps5vk_operation *indexed=&c->operations[1];
    assert(indexed->type==PS5VK_DRAW_INDEXED && indexed->index_count==6 &&
        indexed->first_index==3 && indexed->vertex_offset==-2 && indexed->first_instance==7 &&
        indexed->instance_count==2 && indexed->indices.buffer==indices[0] && indexed->indices.offset==4);
    vkCmdBindIndexBuffer(c,indices[1],0,VK_INDEX_TYPE_UINT32);
    assert(indexed->indices.buffer==indices[0] && indexed->indices.type==VK_INDEX_TYPE_UINT16);
    vkCmdEndRenderPass(c);assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    c->state=PS5VK_PENDING;
    vkDestroyBuffer(&d,indices[0],NULL);assert(d.buffers && c->state==PS5VK_PENDING);
    assert(!d.invalidate(&d,VK_OBJECT_TYPE_BUFFER,indices[0]));
    c->state=PS5VK_EXECUTABLE;
    vkDestroyBuffer(&d,indices[0],NULL);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,&pipeline);
    vkCmdBeginRenderPass(c,&ri,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindIndexBuffer(c,indices[1],0,VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(c,2,1,UINT32_MAX,0,0);
    assert(c->state==PS5VK_INVALID && c->operation_count==1);
    vkDestroyBuffer(&d,indices[1],NULL);vkFreeMemory(&d,memory,NULL);
    vkDestroyCommandPool(&d, p, NULL);
}
static void dynamic_descriptor_recording(void)
{
    struct VkDevice_T d={.buffer_alignment=256,.uniform_buffer_alignment=256};
    struct VkDescriptorPool_T descriptor_pool={.device=&d};
    struct VkDescriptorSet_T set={.pool=&descriptor_pool,.generation=9,
        .defined={VK_TRUE,VK_TRUE,VK_TRUE}};
    set.signature.binding[1]=(struct ps5vk_binding){2,0,VK_SHADER_STAGE_COMPUTE_BIT};
    set.signature.binding[7]=(struct ps5vk_binding){1,2,VK_SHADER_STAGE_COMPUTE_BIT};
    set.signature.type[1]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    set.signature.type[7]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    set.signature.count=3;
    struct VkPipelineLayout_T layout={.device=&d,.set_count=1,.sets={set.signature}};
    struct VkPipeline_T pipeline={.device=&d,.set_count=1,.sets={set.signature},
        .program={.descriptor_set_mask=1,.descriptor_count=3,
            .descriptors={{0,7,0,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC},
                          {0,1,1,4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
                          {0,1,0,8,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}}}};
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c=command(&d,p);VkDescriptorSet handle=&set;
    uint32_t offsets[]={256,512,768};
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_COMPUTE,&pipeline);
    vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_COMPUTE,&layout,0,1,&handle,3,offsets);
    vkCmdDispatch(c,1,1,1);
    assert(c->state==PS5VK_RECORDING && c->operation_count==1);
    assert(c->operations[0].descriptor_dynamic_offsets[0]==768 &&
        c->operations[0].descriptor_dynamic_offsets[1]==512 &&
        c->operations[0].descriptor_dynamic_offsets[2]==256);
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS &&
        !c->set_dynamic_offsets[0][0] && !c->set_dynamic_offsets[0][2]);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_COMPUTE,&layout,0,1,&handle,2,offsets);
    assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    offsets[1]=257;
    vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_COMPUTE,&layout,0,1,&handle,3,offsets);
    assert(c->state==PS5VK_INVALID);
    vkDestroyCommandPool(&d,p,NULL);
}
static void vertex_binding_lifetime(void)
{
    struct VkDevice_T d={.memory={NULL,allocate,release,cache,cache},
        .buffer_alignment=256,.noncoherent_atom=64,.max_allocation=4096};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=256,.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VkBuffer b;assert(vkCreateBuffer(&d,&bi,NULL,&b)==VK_SUCCESS);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=512};
    VkDeviceMemory m;assert(vkAllocateMemory(&d,&mi,NULL,&m)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,b,m,256)==VK_SUCCESS);
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);VkCommandBuffer c=command(&d,p);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    VkDeviceSize offset=12;vkCmdBindVertexBuffers(c,3,1,&b,&offset);
    assert(c->state==PS5VK_RECORDING && c->vertices[3].buffer==b && c->vertices[3].offset==12);
    assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    c->state=PS5VK_PENDING; /* Lifetime fixture, not real GPU execution. */
    vkDestroyBuffer(&d,b,NULL);assert(d.buffers==b);
    vkFreeMemory(&d,m,NULL);assert(d.memories==m);
    c->state=PS5VK_EXECUTABLE;
    vkDestroyBuffer(&d,b,NULL);assert(!d.buffers && c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS && !c->vertices[3].buffer);
    assert(vkCreateBuffer(&d,&bi,NULL,&b)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,b,m,0)==VK_SUCCESS);
    offset=256;vkCmdBindVertexBuffers(c,0,1,&b,&offset);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    offset=0;vkCmdBindVertexBuffers(c,16,1,&b,&offset);assert(c->state==PS5VK_INVALID);
    vkDestroyCommandPool(&d,p,NULL);vkDestroyBuffer(&d,b,NULL);vkFreeMemory(&d,m,NULL);
}
static void index_binding_lifetime(void)
{
    struct VkDevice_T d={.memory={NULL,allocate,release,cache,cache},
        .buffer_alignment=256,.noncoherent_atom=64,.max_allocation=4096};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=256,.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer b;assert(vkCreateBuffer(&d,&bi,NULL,&b)==VK_SUCCESS);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=512};
    VkDeviceMemory m;assert(vkAllocateMemory(&d,&mi,NULL,&m)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,b,m,256)==VK_SUCCESS);
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);VkCommandBuffer c=command(&d,p);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindIndexBuffer(c,b,2,VK_INDEX_TYPE_UINT16);
    assert(c->state==PS5VK_RECORDING && c->indices.buffer==b && c->indices.offset==2);
    vkCmdBindIndexBuffer(c,b,4,VK_INDEX_TYPE_UINT32);
    assert(c->state==PS5VK_RECORDING && c->indices.type==VK_INDEX_TYPE_UINT32);
    assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    c->state=PS5VK_PENDING; /* Host lifetime fixture, not GPU evidence. */
    vkDestroyBuffer(&d,b,NULL);assert(d.buffers==b);
    vkFreeMemory(&d,m,NULL);assert(d.memories==m);
    c->state=PS5VK_EXECUTABLE;
    vkDestroyBuffer(&d,b,NULL);assert(!d.buffers && c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS && !c->indices.buffer);
    assert(vkCreateBuffer(&d,&bi,NULL,&b)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,b,m,0)==VK_SUCCESS);
    const VkDeviceSize bad[]={1,2,256,UINT64_MAX};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        assert(vkResetCommandBuffer(c,0)==VK_SUCCESS);
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdBindIndexBuffer(c,b,bad[i],VK_INDEX_TYPE_UINT32);
        assert(c->state==PS5VK_INVALID && !c->indices.buffer);
    }
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindIndexBuffer(c,b,0,VK_INDEX_TYPE_UINT8_EXT);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindIndexBuffer(c,NULL,0,VK_INDEX_TYPE_UINT16);assert(c->state==PS5VK_INVALID);
    vkDestroyBuffer(&d,b,NULL);
    bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    assert(vkCreateBuffer(&d,&bi,NULL,&b)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,b,m,0)==VK_SUCCESS);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindIndexBuffer(c,b,0,VK_INDEX_TYPE_UINT16);assert(c->state==PS5VK_INVALID);
    vkDestroyCommandPool(&d,p,NULL);vkDestroyBuffer(&d,b,NULL);vkFreeMemory(&d,m,NULL);
}
static void image_barriers(void)
{
    struct VkDevice_T d={.graphics_enabled=VK_TRUE,.memory={NULL,allocate,release,cache,cache},
        .buffer_alignment=256,.noncoherent_atom=64,.max_allocation=4096};
    VkDeviceMemory memory;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=256};
    assert(vkAllocateMemory(&d,&ai,NULL,&memory)==VK_SUCCESS);
    struct VkImage_T image={.device=&d,.memory=memory,.requirements={.size=256},
        .info={.format=VK_FORMAT_R8_UNORM,.mipLevels=1,.arrayLayers=1,
            .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT}};
    d.images=&image;
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c=command(&d,p);
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=&image,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    assert(c->state==PS5VK_RECORDING && c->operation_count==1);
    assert(c->operations[0].type==PS5VK_IMAGE_BARRIER);
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,NULL,0,NULL,1,&b);
    assert(c->operation_count==2 && c->operations[0].image_barrier.oldLayout==VK_IMAGE_LAYOUT_UNDEFINED);
    assert(c->operations[0].src_stage==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    assert(c->operations[0].dst_stage==VK_PIPELINE_STAGE_TRANSFER_BIT);
    assert(c->operations[1].src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT);
    assert(c->operations[1].dst_stage==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    assert(c->operations[1].src_access==VK_ACCESS_TRANSFER_WRITE_BIT);
    assert(c->operations[1].dst_access==VK_ACCESS_SHADER_READ_BIT);
    assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED); /* Recording is not execution. */
    assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    c->state=PS5VK_PENDING;
    assert(!d.invalidate(&d,VK_OBJECT_TYPE_IMAGE,&image));
    c->state=PS5VK_EXECUTABLE;
    assert(d.invalidate(&d,VK_OBJECT_TYPE_IMAGE,&image) && c->state==PS5VK_INVALID);
    VkImageMemoryBarrier batch[2]={b,b};batch[1].newLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,NULL,0,NULL,2,batch);
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,NULL,0,NULL,1,&b); /* transfer access without transfer stage */
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,0,NULL,0,NULL,1,&b); /* shader read without fragment stage */
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    c->operation_count=PS5VK_MAX_OPERATIONS-1;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,NULL,0,NULL,2,batch);
    assert(c->state==PS5VK_INVALID && c->operation_count==PS5VK_MAX_OPERATIONS-1);

    /* The unchanged upstream smoke triangle records one memory dependency and
     * one image transition in the same call. Accept the exact bounded profile
     * transactionally; a bad member must append neither operation. */
    image.info.format=VK_FORMAT_R8G8B8A8_UNORM;
    image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkMemoryBarrier host_vertex={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
    b=(VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=0,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex=0,.dstQueueFamilyIndex=0,.image=&image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,1,&host_vertex,0,NULL,1,&b);
    assert(c->state==PS5VK_RECORDING && c->operation_count==2 &&
        c->operations[0].type==PS5VK_BARRIER &&
        c->operations[1].type==PS5VK_IMAGE_BARRIER);
    /* Original binding-model CTS uses a standalone WRITE-only transition.
     * Exercise all color subsets, empty scope and generic memory aliases. */
    const VkAccessFlags color_scopes[]={0,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    for(unsigned scope=0;scope<sizeof(color_scopes)/sizeof(color_scopes[0]);++scope) {
        assert(vkResetCommandBuffer(c,0)==VK_SUCCESS && vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        b.dstAccessMask=color_scopes[scope];
        vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,0,NULL,0,NULL,1,&b);
        assert(c->state==PS5VK_RECORDING && c->operation_count==1 &&
            c->operations[0].image_barrier.dstAccessMask==color_scopes[scope] &&
            image.layout==VK_IMAGE_LAYOUT_UNDEFINED);
        assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    }
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS && vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    b.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    assert(c->state==PS5VK_INVALID && !c->operation_count); /* Wrong stage. */
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,NULL,0,NULL,1,&b);
    assert(c->state==PS5VK_INVALID && !c->operation_count); /* Wrong access role. */
    b.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS && vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    b.subresourceRange.levelCount=2;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,1,&host_vertex,0,NULL,1,&b);
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    vkDestroyCommandPool(&d,p,NULL);
    d.images=NULL;vkFreeMemory(&d,memory,NULL);
}
static void push_constant_recording(void)
{
    struct VkDevice_T d={0};
    VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,16};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&range};
    VkPipelineLayout layout;assert(vkCreatePipelineLayout(&d,&li,NULL,&layout)==VK_SUCCESS);
    struct VkPipeline_T pipeline={.device=&d,.program={.push_constant_size=16,.push_constant_sgpr=2}};
    pipeline.push_constant_size=layout->push_constant_size;
    memcpy(pipeline.push_constant_stages,layout->push_constant_stages,
           sizeof(pipeline.push_constant_stages));
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c=command(&d,p);
    uint32_t values[]={1,2,3,4};
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_COMPUTE,&pipeline);
    vkCmdPushConstants(c,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(values),values);
    values[0]=99;
    vkCmdDispatch(c,1,1,1);
    assert(c->state==PS5VK_RECORDING && c->operation_count==1);
    assert(c->operations[0].push_constant_size==16);
    assert(((const uint32_t *)c->operations[0].push_constants)[0]==1);
    uint32_t replacement=17;
    vkCmdPushConstants(c,layout,VK_SHADER_STAGE_COMPUTE_BIT,4,4,&replacement);
    vkCmdDispatch(c,2,1,1);
    assert(c->operation_count==2 &&
        ((const uint32_t *)c->operations[1].push_constants)[1]==17 &&
        ((const uint32_t *)c->operations[0].push_constants)[1]==2);
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_COMPUTE,&pipeline);
    vkCmdDispatch(c,1,1,1);
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPushConstants(c,layout,VK_SHADER_STAGE_VERTEX_BIT,0,4,&replacement);
    assert(c->state==PS5VK_INVALID);
    range=(VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT,0,8};
    VkPipelineLayout incompatible;
    assert(vkCreatePipelineLayout(&d,&li,NULL,&incompatible)==VK_SUCCESS);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_COMPUTE,&pipeline);
    vkCmdPushConstants(c,incompatible,VK_SHADER_STAGE_COMPUTE_BIT,0,8,values);
    vkCmdDispatch(c,1,1,1);
    assert(c->state==PS5VK_INVALID && !c->operation_count);
    vkDestroyPipelineLayout(&d,incompatible,NULL);
    const VkShaderStageFlags optional_stages[]={VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,VK_SHADER_STAGE_GEOMETRY_BIT};
    for(unsigned i=0;i<3;++i) {
        range=(VkPushConstantRange){optional_stages[i],16,16};
        VkPipelineLayout optional;
        assert(vkCreatePipelineLayout(&d,&li,NULL,&optional)==VK_SUCCESS);
        assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
        vkCmdPushConstants(c,optional,optional_stages[i],16,sizeof(values),values);
        assert(c->state==PS5VK_RECORDING && c->push_constants_valid);
        assert(!memcmp(c->push_constants+16,values,sizeof(values)));
        vkCmdPushConstants(c,optional,optional_stages[i],12,4,values);
        assert(c->state==PS5VK_INVALID);
        vkDestroyPipelineLayout(&d,optional,NULL);
    }
    range=(VkPushConstantRange){VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT|
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,0,16};
    VkPipelineLayout overlap;
    assert(vkCreatePipelineLayout(&d,&li,NULL,&overlap)==VK_SUCCESS);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdPushConstants(c,overlap,range.stageFlags,0,16,values);
    assert(c->state==PS5VK_RECORDING);
    unsigned char saved_push[16];memcpy(saved_push,c->push_constants,16);
    uint32_t forbidden=0xbadc0deu;
    vkCmdPushConstants(c,overlap,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,0,4,&forbidden);
    assert(c->state==PS5VK_INVALID && !memcmp(saved_push,c->push_constants,16));
    vkDestroyPipelineLayout(&d,overlap,NULL);
    vkDestroyPipelineLayout(&d,layout,NULL);vkDestroyCommandPool(&d,p,NULL);
}
static void core_dynamic_state_recording(void)
{
    struct VkDevice_T d={0};
    VkCommandPool p=pool(&d,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c=command(&d,p);
    const float blend[4]={0.25f,-2.0f,3.5f,1.0f};
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetLineWidth(c,1.0f);
    vkCmdSetDepthBias(c,-1.5f,0.0f,2.25f);
    vkCmdSetBlendConstants(c,blend);
    vkCmdSetDepthBounds(c,0.25f,0.75f);
    vkCmdSetStencilCompareMask(c,VK_STENCIL_FACE_FRONT_BIT,0x1234u);
    vkCmdSetStencilWriteMask(c,VK_STENCIL_FACE_BACK_BIT,0x5678u);
    vkCmdSetStencilReference(c,VK_STENCIL_FACE_FRONT_AND_BACK,9u);
    assert(c->state==PS5VK_RECORDING && c->dynamic_state_valid==0x7fu &&
        c->operation_count==0);
    assert(c->line_width==1.0f && c->depth_bias_constant==-1.5f &&
        c->depth_bias_clamp==0.0f && c->depth_bias_slope==2.25f);
    assert(!memcmp(c->blend_constants,blend,sizeof(blend)));
    assert(c->min_depth_bounds==0.25f && c->max_depth_bounds==0.75f);
    assert(c->stencil_compare_mask[0]==0x1234u && !c->stencil_compare_mask[1]);
    assert(!c->stencil_write_mask[0] && c->stencil_write_mask[1]==0x5678u);
    assert(c->stencil_reference[0]==9u && c->stencil_reference[1]==9u);
    assert(c->stencil_compare_faces==VK_STENCIL_FACE_FRONT_BIT &&
        c->stencil_write_faces==VK_STENCIL_FACE_BACK_BIT &&
        c->stencil_reference_faces==VK_STENCIL_FACE_FRONT_AND_BACK);
    assert(vkResetCommandBuffer(c,0)==VK_SUCCESS && !c->dynamic_state_valid &&
        c->line_width==1.0f && !c->blend_constants[0] &&
        c->min_depth_bounds==0.0f && c->max_depth_bounds==1.0f &&
        !c->stencil_compare_faces && !c->stencil_write_faces &&
        !c->stencil_reference_faces);

    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetLineWidth(c,2.0f);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetDepthBias(c,0.0f,1.0f,0.0f);assert(c->state==PS5VK_INVALID);
    /* The same clamp is legal once depthBiasClamp is enabled on the device;
     * negative and NaN clamps are values, not errors. */
    d.enabled_features|=PS5VK_FEATURE_DEPTH_BIAS_CLAMP;
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetDepthBias(c,0.0f,1.0f,0.0f);assert(c->state==PS5VK_RECORDING && c->depth_bias_clamp==1.0f);
    vkCmdSetDepthBias(c,2.0f,-0.25f,NAN);
    assert(c->state==PS5VK_RECORDING && c->depth_bias_clamp==-0.25f &&
        c->depth_bias_constant==2.0f && isnan(c->depth_bias_slope));
    assert(vkEndCommandBuffer(c)==VK_SUCCESS);
    d.enabled_features&=~PS5VK_FEATURE_DEPTH_BIAS_CLAMP;
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetBlendConstants(c,NULL);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetDepthBounds(c,-0.1f,1.0f);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetDepthBounds(c,0.75f,0.25f);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetDepthBounds(c,NAN,1.0f);assert(c->state==PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c,&begin_info)==VK_SUCCESS);
    vkCmdSetStencilReference(c,0,1);assert(c->state==PS5VK_INVALID);
    vkDestroyCommandPool(&d,p,NULL);
}
int main(void)
{ operation_reservation_contract(); states(); stage_access_scopes(); recording_and_invalidation(); multi_set_recording(); graphics_recording(); subpass_transitions(); dynamic_descriptor_recording(); vertex_binding_lifetime(); index_binding_lifetime(); image_barriers(); push_constant_recording(); core_dynamic_state_recording(); puts("Command recording/ownership: pass (host only, no submit)"); }

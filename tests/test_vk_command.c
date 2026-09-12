#include "vk_command.h"
#include <assert.h>
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
    struct VkDevice_T d={0};
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
    vkCmdDispatch(cmd,1,1,1);
    assert(cmd->state==PS5VK_RECORDING && cmd->operation_count==1 &&
        cmd->operations[0].sets[0]==&a && cmd->operations[0].sets[2]==&cset &&
        cmd->operations[0].generations[0]==3 && cmd->operations[0].generations[2]==7);
    assert(vkEndCommandBuffer(cmd)==VK_SUCCESS);
    assert(vkResetCommandBuffer(cmd,0)==VK_SUCCESS && vkBeginCommandBuffer(cmd,&begin_info)==VK_SUCCESS);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,&pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,&layout,0,1,&set0,0,NULL);
    vkCmdDispatch(cmd,1,1,1);assert(cmd->state==PS5VK_INVALID);
    vkDestroyCommandPool(&d,p,NULL);
}
static void graphics_recording(void)
{
    /* Structural objects only: no shaders, allocation or GPU execution. */
    struct VkDevice_T d = {.graphics_enabled = VK_TRUE,
        .memory={NULL,allocate,release,cache,cache},.buffer_alignment=256,
        .noncoherent_atom=64,.max_allocation=4096};
    struct VkImage_T image = {.device = &d};
    struct VkImageView_T view = {.device = &d, .image = &image};
    struct VkRenderPass_T pass = {.device = &d, .attachment_count = 1,
        .color = {.attachment = 0}, .depth = {.attachment = VK_ATTACHMENT_UNUSED},
        .attachments = {{.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR}}};
    struct VkFramebuffer_T fb = {.device = &d, .width = 100, .height = 100, .attachment_count = 1,
        .attachments = {&view}, .formats = {VK_FORMAT_B8G8R8A8_UNORM}, .samples = {VK_SAMPLE_COUNT_1_BIT},
        .depth_attachment = VK_ATTACHMENT_UNUSED};
    struct VkPipeline_T pipeline = {.device = &d, .graphics = VK_TRUE,
        .color_format = VK_FORMAT_B8G8R8A8_UNORM};
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
    vkCmdDraw(c, 3, 1, 2, 4);
    assert(c->operations[1].sets[0]==graphics_handle && c->operations[1].generations[0]==9);
    vkCmdEndRenderPass(c);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS && c->operation_count == 3);
    assert(c->operations[1].first_vertex == 2 && c->operations[1].first_instance == 4);
    c->state = PS5VK_PENDING;
    assert(!d.invalidate(&d, VK_OBJECT_TYPE_IMAGE, &image));
    assert(!d.invalidate(&d, VK_OBJECT_TYPE_FRAMEBUFFER, &fb));
    assert(!d.invalidate(&d, VK_OBJECT_TYPE_PIPELINE, &pipeline));
    assert(!d.invalidate(&d, VK_OBJECT_TYPE_DESCRIPTOR_SET, graphics_handle));
    c->state = PS5VK_EXECUTABLE;
    assert(d.invalidate(&d, VK_OBJECT_TYPE_IMAGE_VIEW, &view) && c->state == PS5VK_INVALID);
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS && !c->graphics_pipeline && !c->graphics_sets[0]);
    pipeline.set_count=0;
    ri.clearValueCount = 0;
    vkCmdBeginRenderPass(c, &ri, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_INVALID && !c->operation_count);
    ri.clearValueCount = 1;
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
        .info={.format=VK_FORMAT_R8G8B8A8_UNORM,.mipLevels=1,.arrayLayers=1,
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
    vkDestroyCommandPool(&d,p,NULL);
    d.images=NULL;vkFreeMemory(&d,memory,NULL);
}
int main(void)
{ states(); recording_and_invalidation(); multi_set_recording(); graphics_recording(); vertex_binding_lifetime(); index_binding_lifetime(); image_barriers(); puts("Command recording/ownership: pass (host only, no submit)"); }

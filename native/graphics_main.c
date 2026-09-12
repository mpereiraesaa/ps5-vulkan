/* Experimental integration through API-owned objects. Draw/presentation are
 * opt-in build stages; no complete graphics queue profile or conformance claim. */
#include "graphics_library.h"
#include "draw_prepare_ps5.h"
#include "command_arena_ps5.h"
#include "graphics_sync.h"
#include "triangle_readback.h"
#include "scene_geometry.h"
#include "scene_region.h"
#include "scene_clock.h"
#include "scene_witnesses.h"
#include "present_ps5.h"
#include "ps5log.h"
#include <time.h>
#include <unistd.h>
#include <string.h>
void ps5vk_compute_regression(VkDevice);
static uint64_t scene_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static void fail(const char *call, int rc)
{
    ps5log_printf(PS5LOG_ERR, "PS5VK_GRAPHICS_API_FAIL call=%s rc=%d", call, rc);
    ps5log_close("graphics-api-failed");
    if(rc==VK_ERROR_DEVICE_LOST)for(;;)sleep(1);
    _exit(0);
}
#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) fail(#call,r); } while (0)
struct texture_fixture {
    VkImage image;VkImageView view;VkSampler sampler;VkDeviceMemory memory,upload_memory;
    VkBuffer upload;VkDescriptorPool pool;VkDescriptorSet set;
};
static struct texture_fixture texture_create(VkDevice d,VkDescriptorSetLayout layout)
{
    struct texture_fixture t={0};
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={2,2,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(d,&ii,NULL,&t.image));
    VkMemoryRequirements req;vkGetImageMemoryRequirements(d,t.image,&req);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    CHECK(vkAllocateMemory(d,&mi,NULL,&t.memory));CHECK(vkBindImageMemory(d,t.image,t.memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t.image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=ii.format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(vkCreateImageView(d,&vi,NULL,&t.view));
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(vkCreateSampler(d,&si,NULL,&t.sampler));
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=16,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    CHECK(vkCreateBuffer(d,&bi,NULL,&t.upload));vkGetBufferMemoryRequirements(d,t.upload,&req);
    mi.allocationSize=req.size;CHECK(vkAllocateMemory(d,&mi,NULL,&t.upload_memory));
    CHECK(vkBindBufferMemory(d,t.upload,t.upload_memory,0));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    CHECK(vkCreateDescriptorPool(d,&pi,NULL,&t.pool));
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=t.pool,.descriptorSetCount=1,.pSetLayouts=&layout};
    CHECK(vkAllocateDescriptorSets(d,&ai,&t.set));
    VkDescriptorImageInfo image={t.sampler,t.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=t.set,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image};
    vkUpdateDescriptorSets(d,1,&write,0,NULL);
    return t;
}
static void texture_upload(VkDevice d,struct texture_fixture *t,VkCommandBuffer cb,unsigned frame)
{
    void *mapped;CHECK(vkMapMemory(d,t->upload_memory,0,VK_WHOLE_SIZE,0,&mapped));
    const uint32_t colors[3]={0xff0000ff,0xff00ff00,0xffff0000}; /* RGBA8 */
    for(unsigned i=0;i<4;++i)((uint32_t *)mapped)[i]=colors[(i+frame)%3];
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->upload_memory,.size=VK_WHOLE_SIZE};
    CHECK(vkFlushMappedMemoryRanges(d,1,&range));vkUnmapMemory(d,t->upload_memory);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=t->image,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={2,2,1}};
    vkCmdCopyBufferToImage(cb,t->upload,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&barrier);
    ps5log_printf(PS5LOG_MARK,"PS5VK_TEXTURE_UPLOAD frame=%u pattern=%u width=2 height=2",frame,frame%3);
}
static void texture_destroy(VkDevice d,struct texture_fixture *t)
{
    vkDestroyDescriptorPool(d,t->pool,NULL);vkDestroySampler(d,t->sampler,NULL);
    vkDestroyImageView(d,t->view,NULL);vkDestroyImage(d,t->image,NULL);vkFreeMemory(d,t->memory,NULL);
    vkDestroyBuffer(d,t->upload,NULL);vkFreeMemory(d,t->upload_memory,NULL);
}
static void prepare_recorded_draw(VkDevice d, VkPipeline pipeline, VkRenderPass pass,
    VkPipelineLayout layout,VkDescriptorSetLayout set_layout,unsigned witness_index)
{
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={1920,1080,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImage image; CHECK(vkCreateImage(d,&ii,NULL,&image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(d,image,&req);
    VkDeviceSize bind_offset=req.alignment;
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size+req.alignment};
#if PS5VK_GRAPHICS_PRESENT
    /* Registration control: use the allocation base and the Gears
     * reference's two-buffer allocation envelope. This is
     * NOT an inferred Vulkan image requirement or a proven VideoOut minimum. */
    mi.allocationSize=UINT64_C(0x08000000);
    bind_offset=0;
#endif
    VkDeviceMemory memory; CHECK(vkAllocateMemory(d,&mi,NULL,&memory));
    CHECK(vkBindImageMemory(d,image,memory,bind_offset));
    VkImage first=image;
    VkImage depth_image=VK_NULL_HANDLE;VkImageView depth_view=VK_NULL_HANDLE;VkDeviceMemory depth_memory=VK_NULL_HANDLE;
    if(pipeline->depth_format!=VK_FORMAT_UNDEFINED) {
        VkImageCreateInfo di=ii;di.format=VK_FORMAT_D32_SFLOAT;di.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        CHECK(vkCreateImage(d,&di,NULL,&depth_image));
        VkMemoryRequirements dr;vkGetImageMemoryRequirements(d,depth_image,&dr);
        VkMemoryAllocateInfo da={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=dr.size};
        CHECK(vkAllocateMemory(d,&da,NULL,&depth_memory));CHECK(vkBindImageMemory(d,depth_image,depth_memory,0));
        VkImageViewCreateInfo dv={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=depth_image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=di.format,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}};
        CHECK(vkCreateImageView(d,&dv,NULL,&depth_view));
    }
    struct texture_fixture texture={0};
    if(set_layout)texture=texture_create(d,set_layout);
    VkBuffer vertex_buffer=VK_NULL_HANDLE;VkDeviceMemory vertex_memory=VK_NULL_HANDLE;
    VkBuffer index_buffer=VK_NULL_HANDLE;VkDeviceMemory index_memory=VK_NULL_HANDLE;
    VkDeviceSize index_memory_offset=0;
    VkDeviceSize vertex_memory_offset=0;
    if(pipeline->vertex_binding_count) {
        VkBufferCreateInfo vb={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=PS5VK_GRAPHICS_SCENE?1536:384,.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        CHECK(vkCreateBuffer(d,&vb,NULL,&vertex_buffer));
        VkMemoryRequirements vr;vkGetBufferMemoryRequirements(d,vertex_buffer,&vr);
        VkMemoryAllocateInfo va={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=vr.size+vr.alignment};
        CHECK(vkAllocateMemory(d,&va,NULL,&vertex_memory));
        CHECK(vkBindBufferMemory(d,vertex_buffer,vertex_memory,vr.alignment));
        vertex_memory_offset=vr.alignment;
        void *vertices;CHECK(vkMapMemory(d,vertex_memory,0,VK_WHOLE_SIZE,0,&vertices));
        memset(vertices,0,(size_t)va.allocationSize);
        VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=vertex_memory,.size=VK_WHOLE_SIZE};
        CHECK(vkFlushMappedMemoryRanges(d,1,&flush));vkUnmapMemory(d,vertex_memory);
        VkBufferCreateInfo ib={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=PS5VK_GRAPHICS_SCENE?512:128,.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        CHECK(vkCreateBuffer(d,&ib,NULL,&index_buffer));
        VkMemoryRequirements ir;vkGetBufferMemoryRequirements(d,index_buffer,&ir);
        VkMemoryAllocateInfo im={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=ir.size+ir.alignment};
        CHECK(vkAllocateMemory(d,&im,NULL,&index_memory));
        index_memory_offset=ir.alignment;CHECK(vkBindBufferMemory(d,index_buffer,index_memory,index_memory_offset));
        ps5log_printf(PS5LOG_MARK,"PS5VK_VERTEX_INPUT memory_offset=%llu binding_offset=24 first_vertex=1 stride=24",(unsigned long long)vr.alignment);
    }
    /* Scene resource reuse must also be exercised without presentation. */
    unsigned frames=PS5VK_GRAPHICS_SCENE?180:1;
#if PS5VK_GRAPHICS_PRESENT
    VkImage spare; CHECK(vkCreateImage(d,&ii,NULL,&spare));
    CHECK(vkBindImageMemory(d,spare,memory,UINT64_C(0x04000000)));
    struct ps5vk_native_present presentation={0};
    CHECK(ps5vk_native_present_open(&presentation,d,first,spare));
    frames=PS5VK_GRAPHICS_SCENE?180:6;
#endif
    if(PS5VK_GRAPHICS_SCISSOR_PROBE || PS5VK_GRAPHICS_WITNESSES!=0)frames=1;
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==4)frames=PS5VK_SAMPLER_PROBE_CASES;
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==5)frames=3;
    const uint64_t scene_start=scene_now_ns();
    if(PS5VK_GRAPHICS_CONTINUOUS)ps5log_line(PS5LOG_MARK,"PS5VK_SCENE_LOOP mode=continuous animation=monotonic period_ns=6000000000 pause_us=0 readback=per-frame");
    for(uint64_t sequence=0;PS5VK_GRAPHICS_CONTINUOUS || sequence<frames;++sequence) {
    unsigned frame=(unsigned)(PS5VK_GRAPHICS_CONTINUOUS?sequence%180:sequence);
    if(PS5VK_GRAPHICS_WITNESSES)frame=ps5vk_scene_witnesses[witness_index].frame;
    const uint64_t elapsed=scene_now_ns()-scene_start;
    const float scene_angle=PS5VK_GRAPHICS_CONTINUOUS?ps5vk_scene_angle(elapsed):(float)frame*.04f;
    unsigned scale_quarters=4;
    if(vertex_buffer) {
        static const unsigned scales[3]={4,2,3};scale_quarters=scales[frame%3];
        float triangle[36]={-.7f,-.6f,.4f,1,0,0, .7f,-.6f,.4f,0,1,0, 0,.7f,.4f,0,0,1};
        if(set_layout) {
            triangle[3]=0;triangle[4]=0;triangle[9]=1;triangle[10]=0;
            triangle[15]=.5f;triangle[16]=1;
        }
        for(unsigned v=0;v<3;++v) {
            triangle[v*6]*=(float)scale_quarters/4;
            triangle[v*6+1]*=(float)scale_quarters/4;
        }
        memcpy(triangle+18,triangle,18*sizeof(float));
        for(unsigned v=3;v<6;++v) {
            triangle[v*6+2]=.8f;triangle[v*6+3]=.1f;triangle[v*6+4]=.1f;
        }
        /* Previous draw completed before its presentation; this buffer is not
         * scanout storage. Every new upload is flushed before the next submit. */
        void *vertices;CHECK(vkMapMemory(d,vertex_memory,0,VK_WHOLE_SIZE,0,&vertices));
        struct ps5vk_scene_vertex scene_vertices[PS5VK_SCENE_VERTICES];uint16_t scene_indices[PS5VK_SCENE_INDICES];
        if(PS5VK_GRAPHICS_SCENE) {
            int geometry_rc=PS5VK_GRAPHICS_SCISSOR_PROBE>=3 ?
                ps5vk_scene_probe_triangle(scene_vertices,scene_indices) :
                ps5vk_scene_geometry(scene_vertices,scene_indices,scene_angle);
            if(geometry_rc)fail("scene-geometry",-1);
            if(PS5VK_GRAPHICS_SCISSOR_PROBE==4) {
                float u;int numerator;
                if(ps5vk_scene_sampler_uv(frame,&u,&numerator))fail("sampler-probe-input",-1);
                for(unsigned v=0;v<3;++v){scene_vertices[v].uv_angle[0]=u;scene_vertices[v].uv_angle[1]=.25f;}
                ps5log_printf(PS5LOG_MARK,"PS5VK_SAMPLER_PROBE_INPUT frame=%u u_numerator=%d denominator=8192 v_numerator=2048",frame,numerator);
            }
            memcpy((unsigned char *)vertices+vertex_memory_offset+48,scene_vertices,sizeof(scene_vertices));
            ps5log_printf(PS5LOG_MARK,"PS5VK_SCENE_INPUT frame=%u angle_milliradians=%u vertices=%u indices=%u fixture=%s",
                frame,PS5VK_GRAPHICS_SCISSOR_PROBE>=3?0:(PS5VK_GRAPHICS_CONTINUOUS?(unsigned)(scene_angle*1000):frame*40),PS5VK_GRAPHICS_SCISSOR_PROBE>=3?3u:48u,PS5VK_GRAPHICS_SCISSOR_PROBE>=3?3u:72u,
                PS5VK_GRAPHICS_SCISSOR_PROBE>=3?"planar-triangle":"two-cubes");
        } else memcpy((unsigned char *)vertices+vertex_memory_offset+48,triangle,sizeof(triangle));
        VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=vertex_memory,.size=VK_WHOLE_SIZE};
        CHECK(vkFlushMappedMemoryRanges(d,1,&flush));vkUnmapMemory(d,vertex_memory);
        ps5log_printf(PS5LOG_MARK,"PS5VK_VERTEX_UPDATE frame=%u scale_quarters=%u",frame,scale_quarters);
        void *indices;CHECK(vkMapMemory(d,index_memory,0,VK_WHOLE_SIZE,0,&indices));
        uint16_t ix16[6]={3,4,5,3,4,5};
        uint32_t ix32[6]={3,4,5,3,4,5};
        if(depth_image)for(unsigned i=0;i<6;++i) {
            unsigned vertex=3+i%3+(((i/3)^(frame&1))?3:0);
            ix16[i]=(uint16_t)vertex;ix32[i]=vertex;
        }
        if(depth_image && !PS5VK_GRAPHICS_SCENE)ps5log_printf(PS5LOG_MARK,"PS5VK_DEPTH_INPUT frame=%u near_z=0.4 far_z=0.8 near_first=%u enabled=%u",frame,!(frame&1),pipeline->depth_test);
        unsigned element_bytes=(frame&1)?4:2;
        if(PS5VK_GRAPHICS_SCENE) {
            unsigned char *target=(unsigned char *)indices+index_memory_offset+4+2*element_bytes;
            for(unsigned i=0;i<PS5VK_SCENE_INDICES;++i) {
                uint32_t value=scene_indices[i]+3;uint16_t small=(uint16_t)value;
                memcpy(target+i*element_bytes,element_bytes==4?(void *)&value:(void *)&small,element_bytes);
            }
        } else memcpy((unsigned char *)indices+index_memory_offset+4+2*element_bytes,
            (frame&1)?(const void *)ix32:(const void *)ix16,6*element_bytes);
        VkMappedMemoryRange iflush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=index_memory,.size=VK_WHOLE_SIZE};
        CHECK(vkFlushMappedMemoryRanges(d,1,&iflush));vkUnmapMemory(d,index_memory);
        ps5log_printf(PS5LOG_MARK,"PS5VK_INDEX_INPUT frame=%u bits=%u count=%u first_index=2 base_vertex=-2 binding_offset=4 memory_offset=%llu",
            frame,element_bytes*8,PS5VK_GRAPHICS_SCISSOR_PROBE>=3?3u:(PS5VK_GRAPHICS_SCENE?72u:6u),(unsigned long long)index_memory_offset);
    }
#if PS5VK_GRAPHICS_PRESENT
    unsigned slot=frame&1;
    image=slot ? spare : first;
    bind_offset=slot ? UINT64_C(0x04000000) : 0;
    if(image->display_busy)fail("displayed-slot-write",-1);
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_REUSE_BEGIN frame=%u slot=%u offset=%llu",frame,slot,(unsigned long long)bind_offset);
#endif
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=ii.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; CHECK(vkCreateImageView(d,&vi,NULL,&view));
    VkImageView attachments[2]={view,depth_view};
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=depth_image?2:1,.pAttachments=attachments,.width=1920,.height=1080,.layers=1};
    VkFramebuffer fb; CHECK(vkCreateFramebuffer(d,&fi,NULL,&fb));
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool; CHECK(vkCreateCommandPool(d,&pci,NULL,&pool));
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(d,&ai,&cb));
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb,&bi));
    if(set_layout)texture_upload(d,&texture,cb,PS5VK_GRAPHICS_CONTINUOUS?ps5vk_scene_pattern(elapsed):(PS5VK_GRAPHICS_SCISSOR_PROBE==5?frame:(PS5VK_GRAPHICS_SCENE?frame/60:frame)));
    VkRenderPassBeginInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
        .framebuffer=fb,.renderArea={{0,0},{1920,1080}}};
    VkClearValue clears[2]={0};clears[1].depthStencil.depth=1.0f;
    clears[0].color.float32[3]=1.0f;
    if(depth_image || PS5VK_GRAPHICS_SCENE){ri.clearValueCount=depth_image?2:1;ri.pClearValues=clears;}
    vkCmdBeginRenderPass(cb,&ri,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    if(set_layout)vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&texture.set,0,NULL);
    VkDeviceSize vertex_offset=24;
    unsigned draw_count=1;
    if(vertex_buffer)vkCmdBindVertexBuffers(cb,0,1,&vertex_buffer,&vertex_offset);
    if(index_buffer) {
        vkCmdBindIndexBuffer(cb,index_buffer,4,(frame&1)?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16);
        if(PS5VK_GRAPHICS_SCENE) {
            struct ps5vk_scene_draw draws[2];
            draw_count=ps5vk_scene_draws(PS5VK_GRAPHICS_SCENE_SPLIT,draws);
            if(PS5VK_GRAPHICS_SCISSOR_PROBE>=3){draw_count=1;draws[0]=(struct ps5vk_scene_draw){3,2,-2};}
            if(!draw_count)fail("scene-draw-plan",-1);
            for(unsigned j=0;j<draw_count;++j)
                vkCmdDrawIndexed(cb,draws[j].count,1,draws[j].first_index,draws[j].base_vertex,0);
            ps5log_printf(PS5LOG_MARK,"PS5VK_SCENE_DRAW_PLAN frame=%u split=%u draws=%u total_indices=%u",frame,PS5VK_GRAPHICS_SCENE_SPLIT,draw_count,PS5VK_GRAPHICS_SCISSOR_PROBE>=3?3u:72u);
        } else vkCmdDrawIndexed(cb,6,1,2,-2,0);
    } else vkCmdDraw(cb,3,1,0,0);
    vkCmdEndRenderPass(cb); CHECK(vkEndCommandBuffer(cb));
    unsigned prelude=set_layout?3:0;
    if (cb->operation_count!=prelude+2+draw_count)fail("recorded-operation-count",-1);
    for(unsigned j=0;j<draw_count;++j)
        if(cb->operations[prelude+1+j].type!=(index_buffer?PS5VK_DRAW_INDEXED:PS5VK_DRAW))fail("recorded-draw",-1);
#if PS5VK_GRAPHICS_DRAW
    void *mapped;
    CHECK(vkMapMemory(d,memory,0,VK_WHOLE_SIZE,0,&mapped));
    uint32_t *pixels=(uint32_t *)((unsigned char *)mapped+bind_offset);
    size_t words=req.size/4;
    for(size_t i=0;i<words;++i)pixels[i]=0x55aa11ee;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.offset=bind_offset,.size=req.size};
    CHECK(vkFlushMappedMemoryRanges(d,1,&range));
    VkQueue queue;vkGetDeviceQueue(d,0,0,&queue);
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    if(image->layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
       (depth_image && depth_image->layout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL))
        fail("render-pass-layout-commit",-1);
    CHECK(vkInvalidateMappedMemoryRanges(d,1,&range));
    const uint32_t background=PS5VK_GRAPHICS_SCENE?0xff000000:0x55aa11ee;
    struct ps5vk_triangle_readback stats=ps5vk_triangle_scan(pixels,words,background);
    int far_visible=depth_image && !pipeline->depth_test && !(frame&1);
    int valid=(far_visible?ps5vk_triangle_coverage_valid:ps5vk_triangle_readback_valid)(&stats,(uint32_t)pipeline->viewport.width*scale_quarters/4,
        (uint32_t)pipeline->viewport.height*scale_quarters/4);
    if(PS5VK_GRAPHICS_SCENE)valid=stats.changed>1000 && stats.changed<words && !stats.bad_alpha && !stats.bad_sum;
    if(PS5VK_GRAPHICS_WITNESSES) {
        const struct ps5vk_scene_witness *w=&ps5vk_scene_witnesses[witness_index];
        uint32_t expected_bgra=PS5VK_GRAPHICS_WITNESSES==2?w->off_bgra:w->bgra;
        size_t expected=0,other=0;
        for(size_t i=0;i<words;++i) {
            expected+=pixels[i]==expected_bgra;
            other+=pixels[i]!=0xff000000 && pixels[i]!=expected_bgra;
        }
        valid=stats.changed==1 && expected==1 && other==0;
        ps5log_printf(PS5LOG_MARK,"PS5VK_SCENE_WITNESS index=%u frame=%u x=%u y=%u owner=%u expected_bgra=%08x expected_count=%zu other=%zu changed=%llu valid=%d",
            witness_index,frame,w->x,w->y,w->owner,expected_bgra,expected,other,(unsigned long long)stats.changed,valid);
        ps5log_printf(PS5LOG_MARK,"PS5VK_WITNESS_DEPTH index=%u enabled=%u",witness_index,pipeline->depth_test);
    }
    /* An empty scissor intersection is diagnostic evidence, not scene success.
     * Preserve the ordinary scene gate; permit this control to report and retire. */
    if(PS5VK_GRAPHICS_SCISSOR_PROBE)valid=stats.changed<=
        (uint64_t)pipeline->scissor.extent.width*pipeline->scissor.extent.height && !stats.bad_alpha && !stats.bad_sum;
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_API_READBACK changed_words=%llu total_words=%zu bad_alpha=%llu bad_sum=%llu viewport=%ux%u valid=%d",
        (unsigned long long)stats.changed,words,(unsigned long long)stats.bad_alpha,(unsigned long long)stats.bad_sum,
        (unsigned)pipeline->viewport.width,(unsigned)pipeline->viewport.height,valid);
    if(!valid)fail("triangle-readback",-1);
    if(PS5VK_GRAPHICS_SCISSOR_PROBE) {
        uint64_t selected=0;
        for(unsigned ty=0;ty<8;++ty)for(unsigned tx=0;tx<15;++tx) {
            struct ps5vk_scene_region region;
            if(ps5vk_scene_region_scan(pixels,words,tx,ty,&region))fail("scissor-region-range",-1);
            unsigned changed=region.red+region.green+region.blue+region.unexpected;
            if(tx==6 && ty==3)selected=changed;
            if(changed)ps5log_printf(PS5LOG_MARK,"PS5VK_SCISSOR_BLOCK tile_x=%u tile_y=%u changed=%u unexpected=%u",tx,ty,changed,region.unexpected);
        }
        ps5log_printf(PS5LOG_MARK,"PS5VK_SCISSOR_PROBE mode=%u depth=%u x=%d y=%d width=%u height=%u tile6_3_changed=%llu total_changed=%llu tile_match=%u",
            PS5VK_GRAPHICS_SCISSOR_PROBE,pipeline->depth_test,pipeline->scissor.offset.x,pipeline->scissor.offset.y,
            pipeline->scissor.extent.width,pipeline->scissor.extent.height,
            (unsigned long long)selected,(unsigned long long)stats.changed,
            PS5VK_GRAPHICS_SCISSOR_PROBE==1 && selected>0 && selected==stats.changed);
        /* Report discrepancies without hiding blocks or replacing GPU output.
         * This diagnostic still follows the normal retirement/close path. */
    }
    if(PS5VK_GRAPHICS_SCENE && !PS5VK_GRAPHICS_CONTINUOUS && !PS5VK_GRAPHICS_WITNESSES && frame%45==0) {
        for(unsigned ty=2;ty<6;++ty)for(unsigned tx=6;tx<10;++tx) {
            struct ps5vk_scene_region region;
            if(ps5vk_scene_region_scan(pixels,words,tx,ty,&region))fail("region-range",-1);
            ps5log_printf(PS5LOG_MARK,"PS5VK_SCENE_REGION frame=%u tile_x=%u tile_y=%u black=%u red=%u green=%u blue=%u unexpected=%u",
                frame,tx,ty,region.black,region.red,region.green,region.blue,region.unexpected);
            if(region.unexpected)fail("region-color",-1);
        }
    }
    if(set_layout) {
        size_t histogram[3]={0},unexpected=0;
        for(size_t i=0;i<words;++i) {
            uint32_t p=pixels[i];if(p==background)continue;
            if(p==0xffff0000)++histogram[0];else if(p==0xff00ff00)++histogram[1];
            else if(p==0xff0000ff)++histogram[2];else ++unexpected;
        }
        ps5log_printf(PS5LOG_MARK,"PS5VK_TEXTURE_READBACK frame=%u red=%zu green=%zu blue=%zu unexpected=%zu",
            frame,histogram[0],histogram[1],histogram[2],unexpected);
        if(unexpected || (!PS5VK_GRAPHICS_SCISSOR_PROBE && !PS5VK_GRAPHICS_WITNESSES && !far_visible && (!histogram[0] || !histogram[1] || !histogram[2])))fail("texture-readback",-1);
        /* This triangle's UV domain covers the repeated top-left/bottom-right
         * texel most, top-right second, bottom-left least. A red/blue swap
         * preserves the color set but violates this per-pattern ordering. */
        unsigned dominant=frame%3,second=(frame+1)%3,third=(frame+2)%3;
        if(!PS5VK_GRAPHICS_SCENE && (far_visible ? (histogram[dominant]!=stats.changed || histogram[second] || histogram[third]) :
            !(histogram[dominant]>histogram[second] && histogram[second]>histogram[third])))
            fail("texture-component-order",-1);
        if(!PS5VK_GRAPHICS_SCENE) {
            ps5log_printf(PS5LOG_MARK,"PS5VK_TEXTURE_COMPONENT_ORDER frame=%u dominant=%u valid=1",frame,dominant);
            if(depth_image)ps5log_printf(PS5LOG_MARK,"PS5VK_DEPTH_OCCLUSION frame=%u enabled=%u far_visible=%d valid=1",frame,pipeline->depth_test,far_visible);
        }
    }
    vkUnmapMemory(d,memory);
#if PS5VK_GRAPHICS_PRESENT
    CHECK(ps5vk_native_present_frame(&presentation,slot,sequence+1,PS5VK_GRAPHICS_SCENE?0:1));
    if(PS5VK_GRAPHICS_CONTINUOUS && sequence%120==0)
        ps5log_printf(PS5LOG_MARK,"PS5VK_SCENE_HEARTBEAT frames=%llu elapsed_ns=%llu token=%llu",
            (unsigned long long)(sequence+1),(unsigned long long)(scene_now_ns()-scene_start),(unsigned long long)(sequence+1));
#if defined(PS5VK_GRAPHICS_OBSERVE) && PS5VK_GRAPHICS_OBSERVE
    /* Operator observation only: preserve frame inputs/reference sampling and
     * presentation ownership while slowing 180 frames to at least 10.8 s. */
    if(frame==0)ps5log_line(PS5LOG_MARK,"PS5VK_SCENE_OBSERVATION frames=180 pause_us=60000");
    usleep(60000);
#endif
    if((PS5VK_GRAPHICS_SCISSOR_PROBE==3 || PS5VK_GRAPHICS_SCISSOR_PROBE==5) && !pipeline->depth_test &&
        pipeline->scissor.extent.width==1920 && pipeline->scissor.extent.height==1080) {
        /* Diagnostic observation window: retain the live presentation and its
         * owned storage; this is not an additional rendered frame. */
        ps5log_printf(PS5LOG_MARK,"PS5VK_VISUAL_HOLD seconds=10 fixture=planar-triangle frame=%u rgb_probe=%u",frame,PS5VK_GRAPHICS_SCISSOR_PROBE==5);
        sleep(10);
    }
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_REUSE_END frame=%u slot=%u displayed=%d",frame,slot,presentation.displayed);
#endif
#else
    ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT];
    if (ps5_color_select_runtime_defaults(defaults,sceAgcGetRegisterDefaults())) fail("runtime-defaults",-1);
    struct ps5vk_prepared_draw prepared={0};
    CHECK(ps5vk_native_prepare_draw(d,&cb->operations[1],&ri.renderArea,defaults,&prepared));
    struct ps5vk_command_arena arena={0};
    CHECK(ps5vk_command_arena_create(&arena));
    uint32_t *commands=arena.address, *cursor=commands;
    cursor+=ps5vk_graphics_acquire(cursor,PS5VK_COMMAND_ARENA_WORDS);
    const struct ps5vk_native_graphics_pipeline *native=pipeline->graphics_state;
    if (!native->global_table) fail("global-table",-1);
    /* Still no submission: mapped batch preparation is not execution proof. */
    CHECK(ps5vk_native_emit_draw(&cursor,PS5VK_COMMAND_ARENA_WORDS-(uint32_t)(cursor-commands),
        prepared.state,prepared.state,prepared.bytes,&cb->operations[1],
        (uint32_t)(uintptr_t)native->global_table));
    size_t release_words=ps5vk_graphics_release(cursor,PS5VK_COMMAND_ARENA_WORDS-(uint32_t)(cursor-commands),
        (uintptr_t)ps5vk_command_arena_label(&arena),1);
    if (!release_words) fail("release-packet",-1);
    cursor+=release_words;
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_API_DRAW_PREPARED words=%u cx=%u image_bytes=%llu bind_offset=%llu submit_enabled=0",
        (unsigned)(cursor-commands),prepared.state->cx_count,(unsigned long long)req.size,(unsigned long long)req.alignment);
    ps5vk_native_release_draw(&prepared);
    CHECK(ps5vk_command_arena_release(&arena));
#endif
    vkDestroyCommandPool(d,pool,NULL); vkDestroyFramebuffer(d,fb,NULL); vkDestroyImageView(d,view,NULL);
    }
#if PS5VK_GRAPHICS_PRESENT
    CHECK(ps5vk_native_present_close(&presentation));
    vkDestroyImage(d,spare,NULL);
#endif
    vkDestroyImage(d,first,NULL); vkFreeMemory(d,memory,NULL);
    if(vertex_buffer){vkDestroyBuffer(d,vertex_buffer,NULL);vkFreeMemory(d,vertex_memory,NULL);}
    if(index_buffer){vkDestroyBuffer(d,index_buffer,NULL);vkFreeMemory(d,index_memory,NULL);}
    if(set_layout)texture_destroy(d,&texture);
    if(depth_image){vkDestroyImageView(d,depth_view,NULL);vkDestroyImage(d,depth_image,NULL);vkFreeMemory(d,depth_memory,NULL);}
}
int main(void)
{
    struct timespec ts = {0}; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t boot = (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
    ps5log_config cfg; const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&cfg);
    if (ps5log_load_config(paths,1,&cfg,&loaded)) _exit(0);
    cfg.udp = 0;
    if (ps5log_init(&cfg,"PPSA99994","ps5vk",boot)) _exit(0);
    if (PS5VK_EXIT_CONTROL == 1 || PS5VK_EXIT_CONTROL == 3) {
        /* Diagnostic only: same CRT/network path, no Vulkan/AGC/VideoOut. */
        ps5log_line(PS5LOG_MARK,"PS5VK_BOOT stage=exit-control graphics_initialized=0");
        if (PS5VK_EXIT_CONTROL == 3)
            for (unsigned i=0;i<2100;++i)
                ps5log_printf(PS5LOG_MARK,"PS5VK_EXIT_LOG_LOAD record=%u no_gpu=1",i);
        sleep(3);
        ps5log_close("exit-control-end");
        return 0;
    }
    ps5log_printf(PS5LOG_MARK,"PS5VK_BOOT stage=%s submit_enabled=%d",
        PS5VK_EXIT_CONTROL ? "exit-control-device" : "graphics-api",
        !PS5VK_EXIT_CONTROL && PS5VK_GRAPHICS_DRAW);
    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1; VkPhysicalDevice physical;
    CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
#if PS5VK_GRAPHICS_DRAW
    const VkFormat query_formats[]={VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_D32_SFLOAT,VK_FORMAT_R8G8B8A8_UNORM};
    const VkImageUsageFlags query_usages[]={VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    for(unsigned q=0;q<3;++q) {
        VkImageFormatProperties props;
        CHECK(vkGetPhysicalDeviceImageFormatProperties(physical,query_formats[q],VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,query_usages[q],0,&props));
        if(props.maxExtent.width<1920 || props.maxExtent.height<1080 || props.maxExtent.depth!=1 ||
           props.maxMipLevels!=1 || props.maxArrayLayers!=1 || props.sampleCounts!=VK_SAMPLE_COUNT_1_BIT)
            fail("image-query-profile",-1);
        ps5log_printf(PS5LOG_MARK,"PS5VK_IMAGE_QUERY format=%u usage=%u max_width=%u max_height=%u max_bytes=%llu",
            query_formats[q],query_usages[q],props.maxExtent.width,props.maxExtent.height,
            (unsigned long long)props.maxResourceSize);
    }
    VkPhysicalDeviceProperties device_props;vkGetPhysicalDeviceProperties(physical,&device_props);
    if(device_props.limits.maxImageDimension2D<1920 || device_props.limits.maxVertexInputBindings!=1 ||
       device_props.limits.maxViewports!=1 || device_props.limits.maxColorAttachments!=1 ||
       device_props.limits.maxPerStageDescriptorSamplers!=1 || device_props.limits.maxPerStageDescriptorSampledImages!=1 ||
       device_props.limits.maxDescriptorSetSamplers!=1 || device_props.limits.maxDescriptorSetSampledImages!=1 ||
       !device_props.limits.maxSamplerAllocationCount)
        fail("graphics-limits-profile",-1);
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_LIMITS image_2d=%u framebuffer=%ux%u vertex_stride=%u bindings=%u viewports=%u",
        device_props.limits.maxImageDimension2D,device_props.limits.maxFramebufferWidth,
        device_props.limits.maxFramebufferHeight,device_props.limits.maxVertexInputBindingStride,
        device_props.limits.maxVertexInputBindings,device_props.limits.maxViewports);
    ps5log_printf(PS5LOG_MARK,"PS5VK_SAMPLED_LIMITS stage_samplers=%u stage_images=%u set_samplers=%u set_images=%u allocations=%u",
        device_props.limits.maxPerStageDescriptorSamplers,device_props.limits.maxPerStageDescriptorSampledImages,
        device_props.limits.maxDescriptorSetSamplers,device_props.limits.maxDescriptorSetSampledImages,
        device_props.limits.maxSamplerAllocationCount);
#endif
    float priority=1;
    VkDeviceQueueCreateInfo qi = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice device; CHECK(vkCreateDevice(physical,&di,NULL,&device));
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_DEVICE_CREATED");
    if (PS5VK_EXIT_CONTROL == 2) {
        ps5log_line(PS5LOG_MARK,"PS5VK_EXIT_CONTROL level=device no_submission=1 no_videoout=1");
        sleep(3);
        vkDestroyDevice(device,NULL);
        vkDestroyInstance(instance,NULL);
        ps5log_close("exit-control-device-end");
        return 0;
    }
    VkAttachmentDescription attachment = {.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=PS5VK_GRAPHICS_SCENE?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference colorref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentDescription attachments[2]={attachment,{.format=VK_FORMAT_D32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference depthref={1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&colorref};
    int use_depth=graphics_library.programs[0].key.descriptor_set_count!=0;
    if(use_depth)subpass.pDepthStencilAttachment=&depthref;
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=use_depth?2:1,.pAttachments=attachments,
        .subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass pass; CHECK(vkCreateRenderPass(device,&ri,NULL,&pass));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const struct ps5vk_graphics_key *key=&graphics_library.programs[0].key;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
    if(key->descriptor_set_count) {
        VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount=1,.pBindings=&binding};
        CHECK(vkCreateDescriptorSetLayout(device,&si,NULL,&set_layout));
        li.setLayoutCount=1;li.pSetLayouts=&set_layout;
    }
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device,&li,NULL,&layout));
    const struct ps5vk_graphics_module_key *modules[2]={&key->vertex,&key->fragment};
    VkShaderModule shaders[2]; VkPipelineShaderStageCreateInfo stages[2];
    for (unsigned j=0;j<2;++j) {
        VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=modules[j]->word_count*4,.pCode=modules[j]->words};
        CHECK(vkCreateShaderModule(device,&si,NULL,&shaders[j]));
        stages[j]=(VkPipelineShaderStageCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage=j ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,.module=shaders[j],.pName="main"};
    }
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=key->vertex_binding_count,.pVertexBindingDescriptions=key->vertex_bindings,
        .vertexAttributeDescriptionCount=key->vertex_attribute_count,.pVertexAttributeDescriptions=key->vertex_attributes};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkViewport viewport={0,0,1920,1080,0,1}; VkRect2D scissor={{0,0},{1920,1080}};
    if(PS5VK_GRAPHICS_SCISSOR_PROBE)scissor=(VkRect2D){{768,384},{128,128}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineColorBlendAttachmentState color={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&color};
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.layout=layout,.renderPass=pass,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pViewportState=&vp,.pColorBlendState=&blend};
    VkPipelineDepthStencilStateCreateInfo depth={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_LESS};
    if(use_depth)pi.pDepthStencilState=&depth;
    for (unsigned iteration=0;iteration<(PS5VK_GRAPHICS_WITNESSES?(PS5VK_GRAPHICS_WITNESSES==2?4u:6u):((PS5VK_GRAPHICS_SCISSOR_PROBE==2 || PS5VK_GRAPHICS_SCISSOR_PROBE==3)?10u:(PS5VK_GRAPHICS_SCENE?1u:3u)));++iteration) {
        unsigned witness_index=PS5VK_GRAPHICS_WITNESSES==2 && iteration==3?5:iteration;
#if PS5VK_GRAPHICS_DRAW
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_COMPUTE_CONTROL phase=before-graphics iteration=%u",iteration);
        ps5vk_compute_regression(device);
#endif
        if(PS5VK_GRAPHICS_WITNESSES) {
            depth.depthTestEnable=PS5VK_GRAPHICS_WITNESSES==2?VK_FALSE:VK_TRUE;
            viewport.width=1920;viewport.height=1080;
            scissor=(VkRect2D){{(int32_t)ps5vk_scene_witnesses[witness_index].x,(int32_t)ps5vk_scene_witnesses[witness_index].y},{1,1}};
        } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==4 || PS5VK_GRAPHICS_SCISSOR_PROBE==5) {
            depth.depthTestEnable=VK_FALSE;
            scissor=(VkRect2D){{0,0},{1920,1080}};
        } else if(PS5VK_GRAPHICS_SCISSOR_PROBE>=2) {
            unsigned region=iteration%5;
            depth.depthTestEnable=iteration<5;
            viewport.width=1920;viewport.height=1080;
            scissor=(VkRect2D){{(int32_t)(region%2)*960,(int32_t)(region/2)*540},{960,540}};
            if(region==4)scissor=(VkRect2D){{0,0},{1920,1080}};
        } else {
            depth.depthTestEnable=iteration!=1;
            viewport.width=(float)(1920-iteration*480);viewport.height=(float)(1080-iteration*270);
        }
        VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(device,0,1,&pi,NULL,&pipeline));
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_API_PIPELINE_CREATED iteration=%u",iteration);
        prepare_recorded_draw(device,pipeline,pass,layout,set_layout,witness_index);
        vkDestroyPipeline(device,pipeline,NULL);
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_API_PIPELINE_DESTROYED iteration=%u",iteration);
#if PS5VK_GRAPHICS_DRAW
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_COMPUTE_CONTROL phase=after-graphics iteration=%u",iteration);
        ps5vk_compute_regression(device);
#endif
    }
    for (unsigned j=0;j<2;++j) vkDestroyShaderModule(device,shaders[j],NULL);
    vkDestroyPipelineLayout(device,layout,NULL); vkDestroyRenderPass(device,pass,NULL);
    if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    if (PS5VK_SHELL_CLOSE)
        ps5log_line(PS5LOG_MARK,"PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1");
    ps5log_close("graphics-api-end");
    /* Bounded validation variant: application cleanup is complete, but process
     * termination belongs to Close Game. This does not fix SYS_exit and is not
     * the eventual continuous interactive scene. */
    if (PS5VK_SHELL_CLOSE) for (;;) sleep(1);
    /* Return through app_crt: it invokes catchReturnFromMain and exit(),
     * including the registered loader teardown. _exit bypasses that path. */
    return 0;
}

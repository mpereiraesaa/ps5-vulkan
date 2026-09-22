/* Experimental integration through API-owned objects. Draw/presentation are
 * opt-in build stages; no complete graphics queue profile or conformance claim. */
#include "graphics_library.h"
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
#include "runtime_graphics_spirv.h"
#endif
#include "draw_prepare_ps5.h"
#include "command_arena_ps5.h"
#include "graphics_sync.h"
#include "graphics_limits.h"
#include "targets_ps5.h"
#include "input_attachment_probe.h"
#include "fragment_store_probe.h"
#include "dual_source_probe.h"
#include "two_mrt_probe.h"
#include "sample_rate_probe.h"
#include "input_attachment_gate.h"
#include "multiview_witness.h"
#include "clip_cull_witness.h"
#include "geometry_witness.h"
#include "tess_point_matrix.h"
#include "tess_discard_oracle.h"
/* The private semantic keys the profile pairs producer and consumer words on
 * (the clip/cull distance registers among them). */
#include "libpsbc/psbc_compile.h"
#include "vk_render_pass.h"
#include "color_detile.h"
#include "compilation_cache.h"

#include "vk_image_transfer.h"
#include "texture_layout.h"
#include "triangle_readback.h"
#include "scene_geometry.h"
#include "scene_region.h"
#include "sampler_core_probe.h"
#include "sampled_format_probe.h"
#include "integer_sampled_probe.h"
#include "vertex_format_probe.h"
#include "scene_clock.h"
#include "scene_witnesses.h"
#include "present_ps5.h"
#include "ps5log.h"
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY
extern int32_t sceAgcDriverGetTFRing(uint64_t *,uint32_t *);
extern int32_t sceAgcDriverSetTFRing(uint64_t,uint32_t);
extern int32_t sceAgcDriverGetHsOffchipParam(uint16_t *,uint16_t *);
#endif
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 3
#include "tess_ring_lease.h"
static int tess_ring_get(void *unused,uint64_t *address,uint32_t *size)
{ (void)unused;return sceAgcDriverGetTFRing(address,size); }
static int tess_ring_set(void *unused,uint64_t address,uint32_t size)
{ (void)unused;return sceAgcDriverSetTFRing(address,size); }
#endif
#if defined(PS5VK_TESS_PROBE) && PS5VK_TESS_PROBE
extern int32_t __real_sceAgcInit(uint32_t);
int32_t __wrap_sceAgcInit(void *unused_state, uint32_t unused_size)
{
    (void)unused_state;
    (void)unused_size;
    const int32_t rc=__real_sceAgcInit(11);
    ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_INIT_SCALAR value=11 rc=%d",rc);
    return rc;
}
#endif
#ifndef PS5VK_IMAGE_TARGET
#define PS5VK_IMAGE_TARGET 0
#endif
#ifndef PS5VK_MIP_VIEW_BASE
#define PS5VK_MIP_VIEW_BASE 0
#endif
#ifndef PS5VK_MIP_FORCE_LOD
#define PS5VK_MIP_FORCE_LOD -1
#endif
#ifndef PS5VK_MIP_LOD_BIAS
#define PS5VK_MIP_LOD_BIAS 0
#endif
#ifndef PS5VK_RUNTIME_GRAPHICS
#define PS5VK_RUNTIME_GRAPHICS 0
#endif
#ifndef PS5VK_INPUT_ATTACHMENT_PROBE
#define PS5VK_INPUT_ATTACHMENT_PROBE 0
#endif
#ifndef PS5VK_FRAGMENT_STORE_PROBE
#define PS5VK_FRAGMENT_STORE_PROBE 0
#endif
#ifndef PS5VK_DUAL_SOURCE_PROBE
#define PS5VK_DUAL_SOURCE_PROBE 0
#endif
#ifndef PS5VK_SAMPLE_RATE_PROBE
#define PS5VK_SAMPLE_RATE_PROBE 0
#endif
#ifndef PS5VK_TWO_MRT_PROBE
#define PS5VK_TWO_MRT_PROBE 0
#endif
#ifndef PS5VK_CLIP_CULL_PROBE
#define PS5VK_CLIP_CULL_PROBE 0
#endif
#ifndef PS5VK_GEOMETRY_PROBE
#define PS5VK_GEOMETRY_PROBE 0
#endif
/* The tessellation witness: two triangle patches whose evaluation half paints
 * the quantised tessCoord field, so the tessellator's levels are observable in
 * the image the oracle derives. */
#ifndef PS5VK_TESS_PROBE
#define PS5VK_TESS_PROBE 0
#endif
#ifndef PS5VK_TESS_NO_DRAW
#define PS5VK_TESS_NO_DRAW 0
#endif
/* ONE materially distinct tessellation candidate per executable. The engine
 * state a faulting draw leaves behind invalidates anything measured after it
 * in the same process, and a runtime loop over variants that mutates the
 * pipeline's launch state cannot be attributed to a build at all, so the
 * choice is made at compile time and the artifact IS the variant:
 *   1 = control A - TCS writes legal nonzero levels (outer 2/2/2, inner 1)
 *                   and nothing else; TES position/colour are pure functions
 *                   of gl_TessCoord, so no user TCS output is consumed.
 *   2 = control B - the same pipeline with legal ZERO outer levels, which
 *                   discards the patch; its own artifact, never a second draw
 *                   after a faulting one.
 *   3 = witness C - TCS writes per-vertex/per-patch data that TES reads back
 *                   through off-chip storage.
 * Variants that disable required stages or draw a tessellation pipeline as a
 * triangle list are deliberately NOT available: they are invalid hardware
 * combinations that can inspect packet construction but can never establish
 * Vulkan behaviour, so they must not be able to become acceptance evidence. */
#ifndef PS5VK_TESS_VARIANT
#define PS5VK_TESS_VARIANT 0
#endif
/* The source-candidate identity the build injects. The run logs it and the
 * deployed package's own sha256 is verified host-side after the transfer and
 * the mount refresh, which binds run -> artifact -> source. A log boot id is
 * a process token, not a cryptographic artifact identity. */
#ifndef PS5VK_TESS_BUILD_ID
#define PS5VK_TESS_BUILD_ID "unset"
#endif
/* Bounded diagnostic mode for the geometry witness: report every case's outcome
 * in one run instead of stopping at the first failing verdict. The shipping
 * profile keeps the fail-fast behaviour, because a witness that stops at the
 * first failure is the one a promotion gate should judge; this exists so a
 * single diagnostic run can show the whole table - including the case that has
 * lost the device before - without the first failure hiding the rest. */
#ifndef PS5VK_GEOMETRY_ORDER_PROBE
#define PS5VK_GEOMETRY_ORDER_PROBE 0
#endif
#if defined(PS5VK_LAYER_PROBE) && PS5VK_LAYER_PROBE
/* Slice A measurement: the pattern seeded into the allocation slot no
 * attachment is bound to, so "the neighbouring layer is untouched" is a
 * measured fact rather than whatever the allocator returned. */
#define PS5VK_LAYER_SENTINEL UINT32_C(0x5a5a5a5a)
#endif
static const char *layered_target_name(void)
{
    switch(PS5VK_IMAGE_TARGET) {
    case 1:return "2d-array";
    case 2:return "cube";
    case 3:return "3d";
    case 4:return "1d";
    case 5:return "1d-array";
    default:return "none";
    }
}
void ps5vk_compute_regression(VkDevice);
static uint64_t scene_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static unsigned diagnostic_iterations(void)
{
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==13)return 4;
    if(PS5VK_GRAPHICS_WITNESSES)
        return PS5VK_GRAPHICS_WITNESSES==2?4u:6u;
    switch(PS5VK_GRAPHICS_SCISSOR_PROBE) {
    case 10: return PS5VK_INTEGER_SAMPLED_CASES_PER_SIGN;
    case 9: return PS5VK_SAMPLED_FORMAT_CASES*PS5VK_SAMPLED_FORMAT_FILTER_TRIALS;
    case 8: return PS5VK_VERTEX_FORMAT_CASES;
    case 7: return PS5VK_SAMPLED_FORMAT_CASES;
    case 6: return PS5VK_SAMPLER_CORE_CASES;
    case 2:
    case 3: return 10u;
    default: return PS5VK_GRAPHICS_SCENE?1u:3u;
    }
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
    VkFormat format;uint32_t width,height,slices,mip_levels,upload_bytes;
    VkImageType image_type;VkImageViewType view_type;
};
static struct texture_fixture texture_create(VkDevice d,VkDescriptorSetLayout layout,unsigned probe_case)
{
    struct texture_fixture t={0};
    VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width=2,height=2,slices=1,bytes_per_texel=4;
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==10) {
        struct ps5vk_integer_sampled_case c;
        if(ps5vk_integer_sampled_case(PS5VK_INTEGER_SAMPLED_SIGN==2,
                                      probe_case,&c))
            fail("integer-sampled-case",-1);
        format=c.format;
        bytes_per_texel=c.bytes_per_texel;
        if(c.bytes_per_texel==1)width=4;
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==7 || PS5VK_GRAPHICS_SCISSOR_PROBE==9) {
        struct ps5vk_sampled_format_case c;
        unsigned case_index=PS5VK_GRAPHICS_SCISSOR_PROBE==9?
            probe_case/PS5VK_SAMPLED_FORMAT_FILTER_TRIALS:probe_case;
        if(ps5vk_sampled_format_case(case_index,&c))fail("sampled-format-case",-1);
        format=c.format;
        bytes_per_texel=c.bytes_per_texel;
        /* CP DMA copies DWORD-aligned rows. A four-wide R8 solid fixture keeps
         * its 4-byte rows native without changing the full-screen oracle. */
        if(c.bytes_per_texel==1)width=4;
    }
    VkImageType image_type=VK_IMAGE_TYPE_2D;
    VkImageViewType view_type=VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateFlags image_flags=0;
    if(PS5VK_IMAGE_TARGET) {
        width=height=64;slices=PS5VK_IMAGE_TARGET==2?6:3;
        if(PS5VK_IMAGE_TARGET>=4) {
            width=PS5VK_IMAGE_TARGET==4?192:64;height=1;
            slices=PS5VK_IMAGE_TARGET==4?1:3;
        }
        image_type=PS5VK_IMAGE_TARGET==3?VK_IMAGE_TYPE_3D:
            (PS5VK_IMAGE_TARGET>=4?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D);
        view_type=PS5VK_IMAGE_TARGET==1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:
            (PS5VK_IMAGE_TARGET==2?VK_IMAGE_VIEW_TYPE_CUBE:
            (PS5VK_IMAGE_TARGET==3?VK_IMAGE_VIEW_TYPE_3D:
            (PS5VK_IMAGE_TARGET==4?VK_IMAGE_VIEW_TYPE_1D:VK_IMAGE_VIEW_TYPE_1D_ARRAY)));
        if(PS5VK_IMAGE_TARGET==2)image_flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    uint32_t mip_levels=PS5VK_GRAPHICS_SCISSOR_PROBE==12?3u:1u;
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==12){width=64;height=64;slices=1;}
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=image_flags,
        .imageType=image_type,.format=format,
        .extent={width,height,image_type==VK_IMAGE_TYPE_3D?slices:1},
        .mipLevels=mip_levels,.arrayLayers=image_type==VK_IMAGE_TYPE_3D?1:slices,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(d,&ii,NULL,&t.image));
    VkMemoryRequirements req;vkGetImageMemoryRequirements(d,t.image,&req);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    CHECK(vkAllocateMemory(d,&mi,NULL,&t.memory));CHECK(vkBindImageMemory(d,t.image,t.memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t.image,
        .viewType=view_type,.format=ii.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,
            PS5VK_GRAPHICS_SCISSOR_PROBE==12?PS5VK_MIP_VIEW_BASE:0,
            PS5VK_GRAPHICS_SCISSOR_PROBE==12 && PS5VK_MIP_VIEW_BASE?1:mip_levels,0,
            image_type==VK_IMAGE_TYPE_3D?1:slices}};
    CHECK(vkCreateImageView(d,&vi,NULL,&t.view));
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==10) {
        si.magFilter=si.minFilter=VK_FILTER_NEAREST;
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==6) {
        struct ps5vk_sampler_core_case c;
        if(ps5vk_sampler_core_case(probe_case,&c))fail("sampler-core-case",-1);
        si.addressModeU=si.addressModeV=si.addressModeW=c.address_mode;
        si.borderColor=c.border_color;
        si.magFilter=c.mag_filter;
        si.minFilter=c.min_filter;
        if(c.minification)si.maxLod=15.0f;
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==9) {
        si.magFilter=si.minFilter=(probe_case%PS5VK_SAMPLED_FORMAT_FILTER_TRIALS)?
            VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==12) {
        si.magFilter=si.minFilter=VK_FILTER_NEAREST;
        si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.mipLodBias=(float)PS5VK_MIP_LOD_BIAS;
        si.maxLod=2.0f;
        if(PS5VK_MIP_FORCE_LOD>=0)
            si.minLod=si.maxLod=(float)PS5VK_MIP_FORCE_LOD;
    }
    CHECK(vkCreateSampler(d,&si,NULL,&t.sampler));
    if(mip_levels==1)t.upload_bytes=width*height*slices*bytes_per_texel;
    else for(uint32_t level=0;level<mip_levels;++level) {
        uint32_t w=width>>level?width>>level:1,h=height>>level?height>>level:1;
        t.upload_bytes+=w*h*slices*bytes_per_texel;
    }
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=t.upload_bytes,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
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
    t.format=format;t.width=width;t.height=height;t.slices=slices;t.mip_levels=mip_levels;
    t.image_type=image_type;t.view_type=view_type;
    return t;
}
static void texture_upload(VkDevice d,struct texture_fixture *t,VkCommandBuffer cb,
    unsigned frame,unsigned probe_case)
{
    void *mapped;CHECK(vkMapMemory(d,t->upload_memory,0,VK_WHOLE_SIZE,0,&mapped));
    memset(mapped,0,t->upload_bytes);
    const uint32_t colors[3]={0xff0000ff,0xff00ff00,0xffff0000}; /* RGBA8 */
    unsigned checkerboard=0;
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==6) {
        struct ps5vk_sampler_core_case c;
        if(ps5vk_sampler_core_case(probe_case,&c))fail("sampler-core-case",-1);
        checkerboard=c.checkerboard;
    }
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==10) {
        struct ps5vk_integer_sampled_case c;
        if(ps5vk_integer_sampled_case(PS5VK_INTEGER_SAMPLED_SIGN==2,
                                      probe_case,&c) || c.format!=t->format)
            fail("integer-sampled-case",-1);
        for(unsigned pixel=0;pixel<t->width*t->height;++pixel)
            memcpy((unsigned char *)mapped+pixel*c.bytes_per_texel,
                   c.texel,c.bytes_per_texel);
        ps5log_printf(PS5LOG_MARK,"PS5VK_INTEGER_SAMPLED_INPUT case=%u name=%s sign=%s format=%u components=%u bytes_per_texel=%u expected_bgra=%08x",
            probe_case,c.name,PS5VK_INTEGER_SAMPLED_SIGN==2?"sint":"uint",
            c.format,c.components,c.bytes_per_texel,c.expected_bgra);
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==7 || PS5VK_GRAPHICS_SCISSOR_PROBE==9) {
        struct ps5vk_sampled_format_case c;
        unsigned case_index=PS5VK_GRAPHICS_SCISSOR_PROBE==9?
            probe_case/PS5VK_SAMPLED_FORMAT_FILTER_TRIALS:probe_case;
        if(ps5vk_sampled_format_case(case_index,&c) || c.format!=t->format)
            fail("sampled-format-case",-1);
        if(PS5VK_GRAPHICS_SCISSOR_PROBE==9) {
            uint8_t black[16],white[16];uint32_t nearest_bgra,linear_bgra;
            if(ps5vk_sampled_format_filter_texels(case_index,black,white,
                                                  &nearest_bgra,&linear_bgra))
                fail("sampled-format-filter-texels",-1);
            for(unsigned pixel=0;pixel<t->width*t->height;++pixel) {
                unsigned x=pixel%t->width,y=pixel/t->width;
                const uint8_t *texel=((x+y)&1)?white:black;
                memcpy((unsigned char *)mapped+pixel*c.bytes_per_texel,
                    texel,c.bytes_per_texel);
            }
            ps5log_printf(PS5LOG_MARK,"PS5VK_SAMPLED_FILTER_INPUT trial=%u case=%u name=%s format=%u filter=%s bytes_per_texel=%u expected_bgra=%08x",
                probe_case,case_index,c.name,c.format,
                (probe_case%PS5VK_SAMPLED_FORMAT_FILTER_TRIALS)?"linear":"nearest",
                c.bytes_per_texel,(probe_case%PS5VK_SAMPLED_FORMAT_FILTER_TRIALS)?
                    linear_bgra:nearest_bgra);
        } else {
            for(unsigned pixel=0;pixel<t->width*t->height;++pixel)
                memcpy((unsigned char *)mapped+pixel*c.bytes_per_texel,c.texel,c.bytes_per_texel);
            ps5log_printf(PS5LOG_MARK,"PS5VK_SAMPLED_FORMAT_INPUT case=%u name=%s format=%u bytes_per_texel=%u expected_bgra=%08x",
                probe_case,c.name,c.format,c.bytes_per_texel,c.expected_bgra);
        }
    } else if(checkerboard) {
        const uint32_t pixels[4]={0xff000000,0xffffffff,0xffffffff,0xff000000};
        memcpy(mapped,pixels,sizeof(pixels));
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==12) {
        size_t offset=0;
        for(uint32_t level=0;level<t->mip_levels;++level) {
            uint32_t w=t->width>>level?t->width>>level:1;
            uint32_t h=t->height>>level?t->height>>level:1;
            for(uint32_t i=0;i<w*h;++i)
                ((uint32_t *)((unsigned char *)mapped+offset))[i]=colors[level];
            offset+=(size_t)w*h*sizeof(uint32_t);
        }
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_MIPMAP_INPUT levels=%u view_base=%u lod_bias=%d width=%u height=%u colors=red,green,blue bytes=%u",
            t->mip_levels,PS5VK_MIP_VIEW_BASE,PS5VK_MIP_LOD_BIAS,
            t->width,t->height,t->upload_bytes);
    } else if(PS5VK_IMAGE_TARGET) {
        const unsigned pixels=t->width*t->height;
        for(unsigned slice=0;slice<t->slices;++slice)
            for(unsigned i=0;i<pixels;++i)
                ((uint32_t *)mapped)[slice*pixels+i]=
                    PS5VK_IMAGE_TARGET==4?colors[(3u*i)/pixels]:colors[slice%3];
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_LAYERED_INPUT target=%s slices=%u width=%u height=%u",
            layered_target_name(),
            t->slices,t->width,t->height);
    } else for(unsigned i=0;i<4;++i)((uint32_t *)mapped)[i]=colors[(i+frame)%3];
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->upload_memory,.size=VK_WHOLE_SIZE};
    CHECK(vkFlushMappedMemoryRanges(d,1,&range));vkUnmapMemory(d,t->upload_memory);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=t->image,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t->mip_levels,0,
            t->image_type==VK_IMAGE_TYPE_3D?1:t->slices}};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copies[PS5VK_MAX_TEXTURE_MIP_LEVELS]={0};
    VkDeviceSize offset=0;
    for(uint32_t level=0;level<t->mip_levels;++level) {
        uint32_t w=t->width>>level?t->width>>level:1;
        uint32_t h=t->height>>level?t->height>>level:1;
        copies[level]=(VkBufferImageCopy){.bufferOffset=offset,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,
                t->image_type==VK_IMAGE_TYPE_3D?1:t->slices},
            .imageExtent={w,h,t->image_type==VK_IMAGE_TYPE_3D?
                (t->slices>>level?t->slices>>level:1):1}};
        offset+=(VkDeviceSize)w*h*(t->image_type==VK_IMAGE_TYPE_3D?
            copies[level].imageExtent.depth:t->slices)*4u;
    }
    vkCmdCopyBufferToImage(cb,t->upload,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        t->mip_levels,copies);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&barrier);
    ps5log_printf(PS5LOG_MARK,"PS5VK_TEXTURE_UPLOAD frame=%u pattern=%s width=%u height=%u slices=%u levels=%u format=%u",
        frame,PS5VK_GRAPHICS_SCISSOR_PROBE==10?"integer-sampled-solid":
        (PS5VK_GRAPHICS_SCISSOR_PROBE==9?"sampled-format-checkerboard":
        (PS5VK_GRAPHICS_SCISSOR_PROBE==7?"sampled-format-solid":
        (PS5VK_GRAPHICS_SCISSOR_PROBE==12?"mipmap-rgb":
        (PS5VK_IMAGE_TARGET?"layered-rgb":(checkerboard?"checkerboard":"rgb-cycle"))))),
        t->width,t->height,t->slices,t->mip_levels,t->format);
}
static void texture_destroy(VkDevice d,struct texture_fixture *t)
{
    vkDestroyDescriptorPool(d,t->pool,NULL);vkDestroySampler(d,t->sampler,NULL);
    vkDestroyImageView(d,t->view,NULL);vkDestroyImage(d,t->image,NULL);vkFreeMemory(d,t->memory,NULL);
    vkDestroyBuffer(d,t->upload,NULL);vkFreeMemory(d,t->upload_memory,NULL);
}
struct binding_fixture {
    VkBuffer buffers[16];
    VkDeviceMemory memory[16];
    VkDeviceSize offsets[16];
    uint32_t mask;
};
static struct binding_fixture binding_fixture_create(VkDevice d,unsigned test)
{
    struct binding_fixture f={0};
    f.mask=test==1?0x8000u:(test==2?0x8008u:0xffffu);
    for(unsigned b=0;b<16;++b) {
        if(!(f.mask&(1u<<b)))continue;
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=512,.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        CHECK(vkCreateBuffer(d,&bi,NULL,&f.buffers[b]));
        VkMemoryRequirements req;vkGetBufferMemoryRequirements(d,f.buffers[b],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=req.size+req.alignment};
        CHECK(vkAllocateMemory(d,&ai,NULL,&f.memory[b]));
        CHECK(vkBindBufferMemory(d,f.buffers[b],f.memory[b],req.alignment));
        void *mapped;CHECK(vkMapMemory(d,f.memory[b],0,VK_WHOLE_SIZE,0,&mapped));
        memset(mapped,0xa5,(size_t)ai.allocationSize);
        f.offsets[b]=25+2*b; /* Every used binding requires alignment repair. */
        const float value[4]={(float)b+1,(float)b+2,(float)b+3,(float)b+4};
        for(unsigned vertex=0;vertex<3;++vertex)
            memcpy((unsigned char *)mapped+req.alignment+f.offsets[b]+
                vertex*(48+4*b)+4*(b%3),value,sizeof(value));
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=f.memory[b],.size=VK_WHOLE_SIZE};
        CHECK(vkFlushMappedMemoryRanges(d,1,&range));vkUnmapMemory(d,f.memory[b]);
    }
    ps5log_printf(PS5LOG_MARK,"PS5VK_BINDINGS_INPUT case=%u mask=%04x declared=16 reversed_locations=1 distinct_buffers=1 odd_offsets=1",
        test,f.mask);
    return f;
}
static void binding_fixture_destroy(VkDevice d,struct binding_fixture *f)
{
    for(unsigned b=0;b<16;++b) {
        vkDestroyBuffer(d,f->buffers[b],NULL);vkFreeMemory(d,f->memory[b],NULL);
    }
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
#if defined(PS5VK_LAYER_PROBE) && PS5VK_LAYER_PROBE
    /* Slice A measurement: the attachment keeps its exact shape, but it is
     * bound to the SECOND of two aligned slots in one allocation, and the
     * first slot is filled with a sentinel before the draw. If the pinned
     * AGC/DCB target path selects by address, the render lands in the slot the
     * image is bound to and the other slot stays untouched - which is the
     * whole question for a layered (multiview) attachment. */
    bind_offset = req.size;
#endif
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size+req.alignment};
#if defined(PS5VK_LAYER_PROBE) && PS5VK_LAYER_PROBE
    mi.allocationSize = 2u * req.size;
#endif
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
        /* The explicit-clear witness owns the depth buffer through
         * vkCmdClearDepthStencilImage instead of a render-pass load op, which
         * Vulkan requires transfer-destination usage for. */
        if(PS5VK_GRAPHICS_SCISSOR_PROBE==14)di.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CHECK(vkCreateImage(d,&di,NULL,&depth_image));
        VkMemoryRequirements dr;vkGetImageMemoryRequirements(d,depth_image,&dr);
        VkMemoryAllocateInfo da={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=dr.size};
#if defined(PS5VK_LAYER_PROBE) && PS5VK_LAYER_PROBE
        /* The depth attachment is measured the same way as the colour one: it
         * is bound to the second slot of a two-slot allocation so the probe can
         * ask whether the depth target path selects by address too. */
        da.allocationSize=2u*dr.size;
#endif
        CHECK(vkAllocateMemory(d,&da,NULL,&depth_memory));
#if defined(PS5VK_LAYER_PROBE) && PS5VK_LAYER_PROBE
        CHECK(vkBindImageMemory(d,depth_image,depth_memory,dr.size));
        {
            void *depth_seed=NULL;
            CHECK(vkMapMemory(d,depth_memory,0,VK_WHOLE_SIZE,0,&depth_seed));
            uint32_t *slot0=(uint32_t *)depth_seed;
            for(size_t i=0;i<(size_t)(dr.size/4);++i)slot0[i]=PS5VK_LAYER_SENTINEL;
            VkMappedMemoryRange depth_flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory=depth_memory,.offset=0,.size=dr.size};
            CHECK(vkFlushMappedMemoryRanges(d,1,&depth_flush));
            vkUnmapMemory(d,depth_memory);
        }
#else
        CHECK(vkBindImageMemory(d,depth_image,depth_memory,0));
#endif
        VkImageViewCreateInfo dv={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=depth_image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=di.format,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}};
        CHECK(vkCreateImageView(d,&dv,NULL,&depth_view));
    }
    struct texture_fixture texture={0};
    if(set_layout)texture=texture_create(d,set_layout,witness_index);
    VkBuffer vertex_buffer=VK_NULL_HANDLE;VkDeviceMemory vertex_memory=VK_NULL_HANDLE;
    VkBuffer index_buffer=VK_NULL_HANDLE;VkDeviceMemory index_memory=VK_NULL_HANDLE;
    VkDeviceSize index_memory_offset=0;
    VkDeviceSize vertex_memory_offset=0;
    struct binding_fixture bindings={0};
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==13)bindings=binding_fixture_create(d,witness_index);
    if(pipeline->vertex_binding_count && PS5VK_GRAPHICS_SCISSOR_PROBE!=13) {
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
        ps5log_printf(PS5LOG_MARK,"PS5VK_VERTEX_INPUT memory_offset=%llu binding_offset=%u first_vertex=0 stride=%u",
            (unsigned long long)vr.alignment,
            PS5VK_GRAPHICS_SCISSOR_PROBE==8?25u:24u,
            pipeline->vertex_binding.stride);
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
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==14)frames=2;
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
            } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==6) {
                struct ps5vk_sampler_core_case c;
                if(ps5vk_sampler_core_case(witness_index,&c))fail("sampler-core-case",-1);
                if(c.minification) {
                    /* This affine UV plane is centered at pixel (960.5,540.5)
                     * and advances by two normalized texture coordinates per
                     * screen pixel.  The 1x1 scissor below therefore selects
                     * an unambiguous minification witness while preserving UV
                     * 0.5 at the sampled fragment. */
                    const float u[3]={-1152.5f,1151.5f,-0.5f};
                    const float v[3]={-648.5f,-648.5f,647.5f};
                    for(unsigned vertex=0;vertex<3;++vertex) {
                        scene_vertices[vertex].uv_angle[0]=u[vertex];
                        scene_vertices[vertex].uv_angle[1]=v[vertex];
                    }
                } else for(unsigned vertex=0;vertex<3;++vertex)
                    scene_vertices[vertex].uv_angle[0]=scene_vertices[vertex].uv_angle[1]=c.uv;
                ps5log_printf(PS5LOG_MARK,"PS5VK_SAMPLER_CORE_INPUT case=%u name=%s uv_milli=%d minification=%u expected_bgra=%08x",
                    witness_index,c.name,(int)(c.uv*1000.0f),c.minification,c.expected_bgra);
            } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==11 ||
                      PS5VK_GRAPHICS_SCISSOR_PROBE==12) {
                if(ps5vk_scene_probe_triangle_uv(scene_vertices))
                    fail("scene-probe-uv",-1);
            } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==9 ||
                      PS5VK_GRAPHICS_SCISSOR_PROBE==10) {
                for(unsigned vertex=0;vertex<3;++vertex)
                    scene_vertices[vertex].uv_angle[0]=scene_vertices[vertex].uv_angle[1]=0.5f;
            }
            memcpy((unsigned char *)vertices+vertex_memory_offset+48,scene_vertices,sizeof(scene_vertices));
            ps5log_printf(PS5LOG_MARK,"PS5VK_SCENE_INPUT frame=%u angle_milliradians=%u vertices=%u indices=%u fixture=%s",
                frame,PS5VK_GRAPHICS_SCISSOR_PROBE>=3?0:(PS5VK_GRAPHICS_CONTINUOUS?(unsigned)(scene_angle*1000):frame*40),PS5VK_GRAPHICS_SCISSOR_PROBE>=3?3u:48u,PS5VK_GRAPHICS_SCISSOR_PROBE>=3?3u:72u,
                PS5VK_GRAPHICS_SCISSOR_PROBE>=3?"planar-triangle":"two-cubes");
        } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==8) {
            struct ps5vk_vertex_format_case c;
            if(ps5vk_vertex_format_case(witness_index,&c) ||
               pipeline->vertex_attributes[0].format!=c.format ||
               pipeline->vertex_binding.stride!=c.bytes)
                fail("vertex-format-case",-1);
            /* This probe isolates vertex conversion. Runtime indexed draws
             * are a separate unsupported combination, so firstVertex=0 maps
             * directly to the three records beginning at the binding offset. */
            /* Vulkan vertex-buffer offsets and strides are byte granular.  An
             * odd base deliberately proves that the native descriptor path
             * does not silently retain its former dword-alignment contract. */
            const size_t start=25u;
            for(unsigned vertex=0;vertex<3;++vertex) {
                unsigned char *record=(unsigned char *)vertices+
                    vertex_memory_offset+start+vertex*pipeline->vertex_binding.stride;
                memcpy(record,c.raw,c.bytes);
            }
            uint32_t raw_word=0;memcpy(&raw_word,c.raw,c.bytes<4?c.bytes:4);
            ps5log_printf(PS5LOG_MARK,"PS5VK_VERTEX_FORMAT_INPUT case=%u name=%s format=%u numeric=%s components=%u bytes=%u stride=%u binding_offset=%zu word=%08x",
                witness_index,c.name,c.format,c.numeric==PS5VK_VERTEX_PROBE_SINT?"sint":
                    (c.numeric==PS5VK_VERTEX_PROBE_UINT?"uint":"float"),
                c.components,c.bytes,pipeline->vertex_binding.stride,start,raw_word);
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
            frame,element_bytes*8,(PS5VK_GRAPHICS_SCISSOR_PROBE>=3 && PS5VK_GRAPHICS_SCISSOR_PROBE!=8)?3u:(PS5VK_GRAPHICS_SCENE?72u:6u),(unsigned long long)index_memory_offset);
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
    if(set_layout)texture_upload(d,&texture,cb,
        PS5VK_GRAPHICS_CONTINUOUS?ps5vk_scene_pattern(elapsed):
        (PS5VK_GRAPHICS_SCISSOR_PROBE==5?frame:(PS5VK_GRAPHICS_SCENE?frame/60:frame)),
        witness_index);
    /* Explicit depth clear witness. The render pass LOADS depth for this
     * scenario, so the only thing that can establish the buffer is this clear,
     * which runs as its OWN submission and must retire before the render pass
     * is recorded: only a completed clear commits the attachment layout the
     * render pass then requires. Geometry sits at z=0.4 and z=0.8 under
     * VK_COMPARE_OP_LESS, so clearing to 1.0 lets the draw through and clearing
     * to 0.0 stops it, and the colour readback reports which happened. */
    const float witness_depth=(PS5VK_GRAPHICS_SCISSOR_PROBE==14 && (frame&1))?0.0f:1.0f;
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==14) {
        if(!depth_image)fail("depth-clear-witness-target",-1);
        const VkImageSubresourceRange depth_range={VK_IMAGE_ASPECT_DEPTH_BIT,0,
            VK_REMAINING_MIP_LEVELS,0,VK_REMAINING_ARRAY_LAYERS};
        VkCommandBuffer clear_cb;
        CHECK(vkAllocateCommandBuffers(d,&ai,&clear_cb));
        CHECK(vkBeginCommandBuffer(clear_cb,&bi));
        VkImageMemoryBarrier acquire={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=depth_image,.subresourceRange=depth_range};
        vkCmdPipelineBarrier(clear_cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&acquire);
        /* stencil = 0x10 on purpose: the member must be ignored for a
         * depth-only range, exactly as the pinned upstream CTS expects. */
        const VkClearDepthStencilValue witness_value={witness_depth,0x10};
        vkCmdClearDepthStencilImage(clear_cb,depth_image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &witness_value,1,&depth_range);
        VkImageMemoryBarrier to_attachment={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=depth_image,.subresourceRange=depth_range};
        vkCmdPipelineBarrier(clear_cb,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0,0,NULL,0,NULL,1,&to_attachment);
        CHECK(vkEndCommandBuffer(clear_cb));
        if(clear_cb->operation_count!=3)fail("depth-clear-witness-record",-1);
        uint32_t witness_word=0;
        if(!ps5vk_depth_clear_word(witness_depth,&witness_word))fail("depth-clear-witness-word",-1);
        VkQueue clear_queue;vkGetDeviceQueue(d,0,0,&clear_queue);
        VkSubmitInfo clear_submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount=1,.pCommandBuffers=&clear_cb};
        CHECK(vkQueueSubmit(clear_queue,1,&clear_submit,VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(clear_queue));
        /* The committed layout is the driver's own proof that the clear
         * submission retired against its GPU completion label. */
        if(depth_image->layout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
            fail("depth-clear-witness-commit",-1);
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_DEPTH_CLEAR_RECORDED frame=%u clear_depth_word=%08x stencil_ignored=0x10 operations=%u committed_layout=%d",
            frame,witness_word,clear_cb->operation_count,(int)depth_image->layout);
    }
    VkRenderPassBeginInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
        .framebuffer=fb,.renderArea={{0,0},{1920,1080}}};
    VkClearValue clears[2]={0};clears[1].depthStencil.depth=1.0f;
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==6 || PS5VK_GRAPHICS_SCISSOR_PROBE==9 ||
       PS5VK_GRAPHICS_SCISSOR_PROBE==10)
        clears[0].color.float32[0]=clears[0].color.float32[1]=clears[0].color.float32[2]=0.25f;
    clears[0].color.float32[3]=1.0f;
    if(depth_image || PS5VK_GRAPHICS_SCENE){ri.clearValueCount=depth_image?2:1;ri.pClearValues=clears;}
    vkCmdBeginRenderPass(cb,&ri,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    if(set_layout)vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&texture.set,0,NULL);
    VkDeviceSize vertex_offset=PS5VK_GRAPHICS_SCISSOR_PROBE==8?25u:24u;
    unsigned draw_count=1;
    if(vertex_buffer)vkCmdBindVertexBuffers(cb,0,1,&vertex_buffer,&vertex_offset);
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==13)
        for(unsigned b=0;b<16;++b)
            if(bindings.mask&(1u<<b))vkCmdBindVertexBuffers(cb,b,1,&bindings.buffers[b],&bindings.offsets[b]);
    if(index_buffer && PS5VK_GRAPHICS_SCISSOR_PROBE!=8 &&
       !(PS5VK_RUNTIME_GRAPHICS && PS5VK_GRAPHICS_SCISSOR_PROBE==12)) {
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
    unsigned prelude=set_layout?(texture.mip_levels+2):0;
    if (cb->operation_count!=prelude+2+draw_count)fail("recorded-operation-count",-1);
    for(unsigned j=0;j<draw_count;++j)
        if(cb->operations[prelude+1+j].type!=
           ((index_buffer && PS5VK_GRAPHICS_SCISSOR_PROBE!=8 &&
             !(PS5VK_RUNTIME_GRAPHICS && PS5VK_GRAPHICS_SCISSOR_PROBE==12))?
             PS5VK_DRAW_INDEXED:PS5VK_DRAW))
            fail("recorded-draw",-1);
#if PS5VK_GRAPHICS_DRAW
    void *mapped;
    CHECK(vkMapMemory(d,memory,0,VK_WHOLE_SIZE,0,&mapped));
    uint32_t *pixels=(uint32_t *)((unsigned char *)mapped+bind_offset);
    size_t words=req.size/4;
    for(size_t i=0;i<words;++i)pixels[i]=0x55aa11ee;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.offset=bind_offset,.size=req.size};
#if defined(PS5VK_LAYER_PROBE) && PS5VK_LAYER_PROBE
    /* Seed the slot the attachment is NOT bound to, so "untouched" is a
     * measured fact rather than whatever the allocator happened to return. */
    {
        uint32_t *other_slot=(uint32_t *)mapped;
        for(size_t i=0;i<words;++i)other_slot[i]=PS5VK_LAYER_SENTINEL;
    }
    range.offset=0; range.size=2u*req.size;
#endif
    CHECK(vkFlushMappedMemoryRanges(d,1,&range));
    VkQueue queue;vkGetDeviceQueue(d,0,0,&queue);
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    if(image->layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
       (depth_image && depth_image->layout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL))
        fail("render-pass-layout-commit",-1);
    CHECK(vkInvalidateMappedMemoryRanges(d,1,&range));
    if(set_layout && PS5VK_GRAPHICS_SCISSOR_PROBE==12) {
        struct ps5vk_texture_mip_layout mip_layout;
        void *texture_bytes=NULL;
        if(ps5vk_texture_mip_layout_for_slices(texture.format,texture.width,
            texture.height,texture.slices,texture.mip_levels,&mip_layout))
            fail("mipmap-layout-inspect",-1);
        CHECK(vkMapMemory(d,texture.memory,0,VK_WHOLE_SIZE,0,&texture_bytes));
        VkMappedMemoryRange texture_range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=texture.memory,.size=VK_WHOLE_SIZE};
        CHECK(vkInvalidateMappedMemoryRanges(d,1,&texture_range));
        for(uint32_t level=0;level<texture.mip_levels;++level) {
            uint32_t first=0;
            memcpy(&first,(unsigned char *)texture_bytes+
                mip_layout.levels[level].offset,sizeof(first));
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_MIPMAP_STORAGE level=%u offset=%llu pitch=%u first=%08x",
                level,(unsigned long long)mip_layout.levels[level].offset,
                mip_layout.levels[level].row_pitch,first);
        }
        vkUnmapMemory(d,texture.memory);
    }
    const uint32_t background=(PS5VK_GRAPHICS_SCISSOR_PROBE==6 ||
        PS5VK_GRAPHICS_SCISSOR_PROBE==9 ||
        PS5VK_GRAPHICS_SCISSOR_PROBE==10)?0xff404040:
        (PS5VK_GRAPHICS_SCENE?0xff000000:0x55aa11ee);
    struct ps5vk_triangle_readback stats=ps5vk_triangle_scan(pixels,words,background);
    int far_visible=depth_image && !pipeline->depth_test && !(frame&1);
    int valid=(far_visible?ps5vk_triangle_coverage_valid:ps5vk_triangle_readback_valid)(&stats,(uint32_t)pipeline->viewport.width*scale_quarters/4,
        (uint32_t)pipeline->viewport.height*scale_quarters/4);
#if defined(PS5VK_LAYER_PROBE) && PS5VK_LAYER_PROBE
    /* Slice A measurement, colour role: the attachment is bound to the second
     * slot of a two-slot allocation. The rendered oracle above ran against that
     * slot; here the OTHER slot is checked for the sentinel it was seeded with.
     * valid=1 means the target path rendered into the slot the image was bound
     * to and left the neighbouring slot untouched - i.e. selection is by
     * address, which is what a layered attachment would need. */
    if(frame==0) {
        const uint32_t *other_slot=(const uint32_t *)mapped;
        size_t untouched_mismatches=0;
        for(size_t i=0;i<words;++i)
            untouched_mismatches+=other_slot[i]!=PS5VK_LAYER_SENTINEL;
        /* The question here is WHERE the render landed, not what was drawn:
         * valid=1 means the bound slot changed and its neighbour did not. The
         * probe's own triangle oracle is reported separately because it only
         * applies to the triangle program, not to every control library. */
        const int color_valid=!untouched_mismatches && stats.changed>0;
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_LAYER_TARGET_PROBE role=color slot_bytes=%llu bind_offset=%llu sentinel=%08x "
            "untouched_mismatches=%zu rendered_changed=%llu rendered_oracle=%d valid=%d",
            (unsigned long long)req.size,(unsigned long long)bind_offset,
            (unsigned)PS5VK_LAYER_SENTINEL,untouched_mismatches,
            (unsigned long long)stats.changed,valid,color_valid);
    }
    /* Depth role: the same two-slot question for the depth target path. The
     * depth attachment is bound to the second slot and the first was seeded
     * with the sentinel, so valid=1 means the depth clear/draw landed in the
     * bound slot and left its neighbour alone - the answer to "does depth need
     * distinct routing" at the address level. */
    if(frame==0 && depth_image) {
        VkMemoryRequirements depth_read_requirements;
        vkGetImageMemoryRequirements(d,depth_image,&depth_read_requirements);
        void *depth_bytes=NULL;
        CHECK(vkMapMemory(d,depth_memory,0,VK_WHOLE_SIZE,0,&depth_bytes));
        VkMappedMemoryRange depth_read={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=depth_memory,.offset=0,.size=2u*depth_read_requirements.size};
        CHECK(vkInvalidateMappedMemoryRanges(d,1,&depth_read));
        const uint32_t *depth_slots=(const uint32_t *)depth_bytes;
        const size_t depth_words=(size_t)(depth_read_requirements.size/4);
        size_t depth_untouched_mismatches=0,depth_rendered_changed=0;
        for(size_t i=0;i<depth_words;++i)
            depth_untouched_mismatches+=depth_slots[i]!=PS5VK_LAYER_SENTINEL;
        for(size_t i=0;i<depth_words;++i)
            depth_rendered_changed+=depth_slots[depth_words+i]!=PS5VK_LAYER_SENTINEL;
        const int depth_valid=!depth_untouched_mismatches && depth_rendered_changed>0;
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_LAYER_TARGET_PROBE role=depth slot_bytes=%llu bind_offset=%llu sentinel=%08x "
            "untouched_mismatches=%zu rendered_changed=%zu valid=%d",
            (unsigned long long)depth_read_requirements.size,
            (unsigned long long)depth_read_requirements.size,
            (unsigned)PS5VK_LAYER_SENTINEL,depth_untouched_mismatches,
            depth_rendered_changed,depth_valid);
        vkUnmapMemory(d,depth_memory);
    }
#endif
    if(PS5VK_GRAPHICS_SCENE)valid=stats.changed>1000 && stats.changed<words && !stats.bad_alpha && !stats.bad_sum;
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==14) {
        /* The GPU-visible oracle. depth cleared to 1.0 lets the z=0.4/0.8
         * geometry through VK_COMPARE_OP_LESS and the colour target changes;
         * depth cleared to 0.0 stops every fragment and the target must be
         * untouched. Nothing else differs between the two frames, so a
         * difference here is the explicit clear and nothing else. */
        const int expect_visible=witness_depth>0.5f;
        valid=expect_visible?(stats.changed>1000 && stats.changed<words):
                             (stats.changed==0);
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_DEPTH_CLEAR_WITNESS frame=%u clear_depth_word=%08x expect_visible=%d changed=%llu total=%zu bad_alpha=%llu valid=%d",
            frame,expect_visible?0x3f800000u:0u,expect_visible,
            (unsigned long long)stats.changed,words,(unsigned long long)stats.bad_alpha,valid);
    }
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
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==13) {
        size_t expected=0,other=0;
        for(size_t i=0;i<words;++i) {
            expected+=pixels[i]==UINT32_MAX;
            other+=pixels[i]!=UINT32_MAX && pixels[i]!=background;
        }
        valid=expected==471744u && !other;
        ps5log_printf(PS5LOG_MARK,"PS5VK_BINDINGS_READBACK case=%u mask=%04x expected_white=%zu other=%zu valid=%d",
            witness_index,bindings.mask,expected,other,valid);
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==8) {
        struct ps5vk_vertex_format_case c;size_t expected=0,other=0;
        uint32_t first_other=0;
        if(ps5vk_vertex_format_case(witness_index,&c))fail("vertex-format-case",-1);
        for(size_t i=0;i<words;++i) {
            if(pixels[i]==UINT32_MAX)++expected;
            else if(pixels[i]!=background) {
                if(!other)first_other=pixels[i];
                ++other;
            }
        }
        valid=expected==471744u && !other;
        ps5log_printf(PS5LOG_MARK,"PS5VK_VERTEX_FORMAT_READBACK case=%u name=%s format=%u numeric=%s components=%u expected_white=%zu other=%zu first_other=%08x valid=%d",
            witness_index,c.name,c.format,c.numeric==PS5VK_VERTEX_PROBE_SINT?"sint":
                (c.numeric==PS5VK_VERTEX_PROBE_UINT?"uint":"float"),
            c.components,expected,other,first_other,valid);
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==10) {
        struct ps5vk_integer_sampled_case c;size_t expected=0,other=0;
        uint32_t first_other=0;
        if(ps5vk_integer_sampled_case(PS5VK_INTEGER_SAMPLED_SIGN==2,
                                      witness_index,&c))
            fail("integer-sampled-case",-1);
        for(size_t i=0;i<words;++i) {
            if(pixels[i]==c.expected_bgra)++expected;
            else if(pixels[i]!=background) {
                if(!other)first_other=pixels[i];
                ++other;
            }
        }
        /* The owned fixture is one triangle covering exactly half of 1920x1080. */
        valid=expected==PS5VK_INTEGER_SAMPLED_EXPECTED_PIXELS && !other;
        ps5log_printf(PS5LOG_MARK,"PS5VK_INTEGER_SAMPLED_READBACK case=%u name=%s sign=%s format=%u expected_bgra=%08x expected=%zu other=%zu first_other=%08x valid=%d",
            witness_index,c.name,PS5VK_INTEGER_SAMPLED_SIGN==2?"sint":"uint",
            c.format,c.expected_bgra,expected,other,first_other,valid);
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==7) {
        struct ps5vk_sampled_format_case c;size_t expected=0,other=0;
        uint32_t first_other=0;
        if(ps5vk_sampled_format_case(witness_index,&c))fail("sampled-format-case",-1);
        for(size_t i=0;i<words;++i) {
            if(pixels[i]==c.expected_bgra)++expected;
            else if(pixels[i]!=background) {
                if(!other)first_other=pixels[i];
                ++other;
            }
        }
        valid=expected==373248u && !other;
        ps5log_printf(PS5LOG_MARK,"PS5VK_SAMPLED_FORMAT_READBACK case=%u name=%s format=%u expected_bgra=%08x expected=%zu other=%zu first_other=%08x valid=%d",
            witness_index,c.name,c.format,c.expected_bgra,expected,other,first_other,valid);
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==9) {
        struct ps5vk_sampled_format_case c;size_t expected=0,other=0;
        uint8_t black[16],white[16];
        uint32_t nearest_bgra,linear_bgra,first_other=0;
        unsigned case_index=witness_index/PS5VK_SAMPLED_FORMAT_FILTER_TRIALS;
        unsigned linear=witness_index%PS5VK_SAMPLED_FORMAT_FILTER_TRIALS;
        if(ps5vk_sampled_format_case(case_index,&c) ||
           ps5vk_sampled_format_filter_texels(case_index,black,white,
                                               &nearest_bgra,&linear_bgra))
            fail("sampled-format-filter-case",-1);
        uint32_t expected_bgra=linear?linear_bgra:nearest_bgra;
        for(size_t i=0;i<words;++i) {
            if(pixels[i]==expected_bgra)++expected;
            else if(pixels[i]!=background) {
                if(!other)first_other=pixels[i];
                ++other;
            }
        }
        valid=expected==373248u && !other;
        ps5log_printf(PS5LOG_MARK,"PS5VK_SAMPLED_FILTER_READBACK trial=%u case=%u name=%s format=%u filter=%s expected_bgra=%08x expected=%zu other=%zu first_other=%08x valid=%d",
            witness_index,case_index,c.name,c.format,linear?"linear":"nearest",
            expected_bgra,expected,other,first_other,valid);
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==6) {
        struct ps5vk_sampler_core_case c;size_t expected=0,other=0;
        if(ps5vk_sampler_core_case(witness_index,&c))fail("sampler-core-case",-1);
        for(size_t i=0;i<words;++i) {
            if(pixels[i]==c.expected_bgra)++expected;
            else if(pixels[i]!=background)++other;
        }
        valid=expected==(c.minification?1u:373248u) && !other;
        ps5log_printf(PS5LOG_MARK,"PS5VK_SAMPLER_CORE_READBACK case=%u name=%s expected_bgra=%08x expected=%zu other=%zu valid=%d",
            witness_index,c.name,c.expected_bgra,expected,other,valid);
    } else if(PS5VK_GRAPHICS_SCISSOR_PROBE)valid=stats.changed<=
        (uint64_t)pipeline->scissor.extent.width*pipeline->scissor.extent.height && !stats.bad_alpha && !stats.bad_sum;
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_API_READBACK changed_words=%llu total_words=%zu bad_alpha=%llu bad_sum=%llu viewport=%ux%u valid=%d",
        (unsigned long long)stats.changed,words,(unsigned long long)stats.bad_alpha,(unsigned long long)stats.bad_sum,
        (unsigned)pipeline->viewport.width,(unsigned)pipeline->viewport.height,valid);
    if(!valid)fail("triangle-readback",-1);
    if(PS5VK_GRAPHICS_SCISSOR_PROBE && PS5VK_GRAPHICS_SCISSOR_PROBE!=7 &&
       PS5VK_GRAPHICS_SCISSOR_PROBE!=8 && PS5VK_GRAPHICS_SCISSOR_PROBE!=9) {
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
    if(PS5VK_GRAPHICS_SCENE && PS5VK_GRAPHICS_SCISSOR_PROBE!=6 &&
       PS5VK_GRAPHICS_SCISSOR_PROBE!=7 && PS5VK_GRAPHICS_SCISSOR_PROBE!=9 &&
       PS5VK_GRAPHICS_SCISSOR_PROBE!=10 &&
       !PS5VK_GRAPHICS_CONTINUOUS &&
       !PS5VK_GRAPHICS_WITNESSES && frame%45==0) {
        for(unsigned ty=2;ty<6;++ty)for(unsigned tx=6;tx<10;++tx) {
            struct ps5vk_scene_region region;
            if(ps5vk_scene_region_scan(pixels,words,tx,ty,&region))fail("region-range",-1);
            ps5log_printf(PS5LOG_MARK,"PS5VK_SCENE_REGION frame=%u tile_x=%u tile_y=%u black=%u red=%u green=%u blue=%u unexpected=%u",
                frame,tx,ty,region.black,region.red,region.green,region.blue,region.unexpected);
            if(region.unexpected)fail("region-color",-1);
        }
    }
    if(set_layout && PS5VK_GRAPHICS_SCISSOR_PROBE!=6 &&
       PS5VK_GRAPHICS_SCISSOR_PROBE!=7 && PS5VK_GRAPHICS_SCISSOR_PROBE!=9 &&
       PS5VK_GRAPHICS_SCISSOR_PROBE!=10) {
        size_t histogram[3]={0},unexpected=0;
        for(size_t i=0;i<words;++i) {
            uint32_t p=pixels[i];if(p==background)continue;
            if(p==0xffff0000)++histogram[0];else if(p==0xff00ff00)++histogram[1];
            else if(p==0xff0000ff)++histogram[2];else ++unexpected;
        }
        ps5log_printf(PS5LOG_MARK,"PS5VK_TEXTURE_READBACK frame=%u red=%zu green=%zu blue=%zu unexpected=%zu",
            frame,histogram[0],histogram[1],histogram[2],unexpected);
        if(PS5VK_GRAPHICS_SCISSOR_PROBE==12) {
            int mip_valid=!unexpected;
            if(PS5VK_MIP_LOD_BIAS<0)
                mip_valid=mip_valid && histogram[0]==186624 && !histogram[1] && !histogram[2];
            else if(PS5VK_MIP_LOD_BIAS>0)
                mip_valid=mip_valid && !histogram[0] && !histogram[1] && histogram[2]==186624;
            else
                mip_valid=mip_valid && histogram[0]==103680 && histogram[1]==62208 && histogram[2]==20736;
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_MIPMAP_READBACK levels=%u lod_bias=%d red=%zu green=%zu blue=%zu unexpected=%zu valid=%d",
                texture.mip_levels,PS5VK_MIP_LOD_BIAS,
                histogram[0],histogram[1],histogram[2],unexpected,mip_valid);
            if(!mip_valid)fail("mipmap-readback",-1);
        }
        if(PS5VK_IMAGE_TARGET) {
            int layered_valid=!unexpected && histogram[0] && histogram[1] && histogram[2];
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_LAYERED_READBACK target=%s red=%zu green=%zu blue=%zu unexpected=%zu valid=%d",
                layered_target_name(),
                histogram[0],histogram[1],histogram[2],unexpected,layered_valid);
            if(!layered_valid)fail("layered-texture-readback",-1);
        }
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
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==13)binding_fixture_destroy(d,&bindings);
    if(index_buffer){vkDestroyBuffer(d,index_buffer,NULL);vkFreeMemory(d,index_memory,NULL);}
    if(set_layout)texture_destroy(d,&texture);
    if(depth_image){vkDestroyImageView(d,depth_view,NULL);vkDestroyImage(d,depth_image,NULL);vkFreeMemory(d,depth_memory,NULL);}
}
#if defined(PS5VK_MULTIVIEW_VIEW_PROBE) && PS5VK_MULTIVIEW_VIEW_PROBE
/* T02-D1b: the private six-view witness. One render pass with a real view mask
 * renders one draw into six array layers, a runtime vertex stage that reads
 * gl_ViewIndex gives every layer its own colour and its own depth, and the
 * readback judges each layer separately - identity, order, coverage, guards and
 * aliasing - through the same oracle the host regressions exercise. Nothing is
 * advertised and no query changes; the pass exists only because the diagnostic
 * build lets vkCreateRenderPass accept the mask. */
enum { PS5VK_MULTIVIEW_WITNESS_EXTENT = 64,
       /* One more layer than the pass renders: the trailing layer is a guard the
        * render must never touch, so "no aliasing off the end" is measured. */
       PS5VK_MULTIVIEW_WITNESS_LAYERS = 7,
       PS5VK_MULTIVIEW_WITNESS_MASK = 0x3f };
#define PS5VK_MULTIVIEW_GUARD UINT32_C(0x5a5a5a5a)

static void multiview_witness_image(VkDevice d, VkFormat format, VkImageUsageFlags usage,
    VkDeviceSize *stride_out, VkImage *image_out, VkDeviceMemory *memory_out, void **mapped_out)
{
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=format,.extent={PS5VK_MULTIVIEW_WITNESS_EXTENT,PS5VK_MULTIVIEW_WITNESS_EXTENT,1},
        .mipLevels=1,.arrayLayers=PS5VK_MULTIVIEW_WITNESS_LAYERS,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=usage,.tiling=VK_IMAGE_TILING_OPTIMAL};
    CHECK(vkCreateImage(d,&ii,NULL,image_out));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(d,*image_out,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    CHECK(vkAllocateMemory(d,&ai,NULL,memory_out));
    CHECK(vkBindImageMemory(d,*image_out,*memory_out,0));
    CHECK(vkMapMemory(d,*memory_out,0,VK_WHOLE_SIZE,0,mapped_out));
    /* The whole allocation is the sentinel before anything renders. Only the
     * TRAILING layer is a guard: the six layers the pass owns are loaded with
     * LOAD_OP_DONT_CARE and written by the draw, so neither their padding nor
     * anything else about them is untouched storage - their content is judged by
     * the oracle's counts, never by the sentinel. */
    uint32_t *words=*mapped_out;
    for (VkDeviceSize i=0;i<req.size/4;++i) words[i]=PS5VK_MULTIVIEW_GUARD;
    CHECK(vkFlushMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
        .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=*memory_out,.offset=0,.size=req.size}));
    VkDeviceSize stride=0, alignment=0, bytes=0;
    if (ps5vk_native_layered_storage(format,PS5VK_MULTIVIEW_WITNESS_EXTENT,
        PS5VK_MULTIVIEW_WITNESS_EXTENT,1,&stride,&alignment,&bytes)!=VK_SUCCESS)
        fail("multiview-witness-storage",-1);
    if (!stride || bytes!=stride || stride*PS5VK_MULTIVIEW_WITNESS_LAYERS>req.size)
        fail("multiview-witness-stride",-1);
    *stride_out=stride;
}

static void multiview_view_probe(VkDevice d)
{
    VkDeviceSize color_stride=0,depth_stride=0;
    VkImage color_image,depth_image; VkDeviceMemory color_memory,depth_memory;
    void *color_map=NULL,*depth_map=NULL;
    multiview_witness_image(d,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        &color_stride,&color_image,&color_memory,&color_map);
    multiview_witness_image(d,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        &depth_stride,&depth_image,&depth_memory,&depth_map);
    VkImageSubresourceRange color_range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,PS5VK_MULTIVIEW_WITNESS_VIEWS};
    VkImageSubresourceRange depth_range={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,PS5VK_MULTIVIEW_WITNESS_VIEWS};
    VkImageViewCreateInfo cvi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=color_image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY,.format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange=color_range};
    VkImageViewCreateInfo dvi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=depth_image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY,.format=VK_FORMAT_D32_SFLOAT,.subresourceRange=depth_range};
    VkImageView color_view,depth_view;
    CHECK(vkCreateImageView(d,&cvi,NULL,&color_view));
    CHECK(vkCreateImageView(d,&dvi,NULL,&depth_view));
    VkAttachmentDescription attachments[2]={
        /* DONT_CARE on both attachments: the execute path's CLEAR would DMA-fill
         * the whole VkImage and overwrite the trailing guard layer, and the
         * depth comparison is ALWAYS, so no prior value is needed at all. */
        {.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format=VK_FORMAT_D32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference colorref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthref={1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&colorref,.pDepthStencilAttachment=&depthref};
    const uint32_t view_masks[1]={PS5VK_MULTIVIEW_WITNESS_MASK};
    VkRenderPassMultiviewCreateInfo multiview={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .subpassCount=1,.pViewMasks=view_masks};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.pNext=&multiview,
        .attachmentCount=2,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass pass; CHECK(vkCreateRenderPass(d,&ri,NULL,&pass));
    VkImageView fb_attachments[2]={color_view,depth_view};
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=2,.pAttachments=fb_attachments,
        .width=PS5VK_MULTIVIEW_WITNESS_EXTENT,.height=PS5VK_MULTIVIEW_WITNESS_EXTENT,.layers=1};
    VkFramebuffer fb; CHECK(vkCreateFramebuffer(d,&fi,NULL,&fb));
    struct ps5vk_graphics_module_key modules[2]={
        {.words=ps5vk_runtime_view_index,.word_count=sizeof(ps5vk_runtime_view_index)/4,.entry="main"},
        {.words=ps5vk_runtime_fragment,.word_count=sizeof(ps5vk_runtime_fragment)/4,.entry="main"}};
#if PS5VK_MULTIVIEW_INSTANCE_PROBE
    /* The instance witness: the same six-view scene, with the vertex stage that
     * also bit-tests gl_InstanceIndex against the pinned floor. */
    modules[0]=(struct ps5vk_graphics_module_key){
        .words=ps5vk_runtime_view_index_instance,
        .word_count=sizeof(ps5vk_runtime_view_index_instance)/4,.entry="main"};
#endif
    VkShaderModule shaders[2];
    for (unsigned j=0;j<2;++j) {
        VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=modules[j].word_count*4,.pCode=modules[j].words};
        CHECK(vkCreateShaderModule(d,&si,NULL,&shaders[j]));
    }
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=shaders[0],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=shaders[1],.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkViewport viewport={0,0,PS5VK_MULTIVIEW_WITNESS_EXTENT,PS5VK_MULTIVIEW_WITNESS_EXTENT,0,1};
    VkRect2D scissor={{0,0},{PS5VK_MULTIVIEW_WITNESS_EXTENT,PS5VK_MULTIVIEW_WITNESS_EXTENT}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend_attachment};
    /* Depth is WRITTEN, so the depth layer is evidence about which view rendered
     * into it: the comparison never rejects (the witness vertex stage's depth is
     * a function of the view) and the clear value only fills what is not drawn. */
    VkPipelineDepthStencilStateCreateInfo depth_state={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_ALWAYS};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d,&li,NULL,&layout));
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .layout=layout,.renderPass=pass,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pViewportState=&vp,.pColorBlendState=&blend,
        .pDepthStencilState=&depth_state};
    VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(d,0,1,&pi,NULL,&pipeline));

    /* PRE-SUBMIT GATE. Nothing below this point is allowed to reach the GPU
     * unless the compiled stage really receives a view index, the pass really
     * owns the mask, the mask really expands to the six views in order, and the
     * six attachment targets really address six distinct ordered layers. A
     * mismatch fails here, with no submission at all. */
    const struct ps5vk_native_graphics_pipeline *native=pipeline->graphics_state;
    if(!native || !native->pair || !native->pair->ready)fail("multiview-witness-pipeline",-1);
    const struct ps5vk_runtime_draw_abi *abi=&native->pair->runtime_arguments;
    if(!abi->enabled || abi->view_index_slot==UINT32_MAX ||
       abi->view_index_slot>=abi->vertex_count)fail("multiview-witness-metadata-slot",-1);
    if(!pass->multiview.present || pass->multiview.subpass_count!=1 ||
       pass->multiview.view_masks[0]!=PS5VK_MULTIVIEW_WITNESS_MASK)
        fail("multiview-witness-pass-mask",-1);
    /* The instance witness pins its whole input in the probe, before anything is
     * recorded: EXACTLY one instance whose firstInstance is the floor this slice
     * is about, and the same value the oracle and the shader bit-test agree on.
     * A mismatch fails here, with no submission. */
    const uint32_t first_instance=
#if PS5VK_MULTIVIEW_INSTANCE_PROBE
        UINT32_C(0x07ffffff);
#else
        0u;
#endif
    const uint32_t instance_count=1u;
#if PS5VK_MULTIVIEW_INSTANCE_PROBE
    if(first_instance!=ps5vk_multiview_witness_instance() ||
       !ps5vk_multiview_witness_instance_exact(first_instance) ||
       (uint32_t)(float)first_instance==first_instance)
        fail("multiview-witness-instance-pin",-1);
    /* gl_InstanceIndex is the compiler's instance id PLUS the start-instance
     * user-SGPR, so a metadata regression that dropped that slot would reach the
     * GPU with a value the probe cannot distinguish from a wrong one. The gate
     * therefore requires the slot itself, in range, and relies on the existing
     * ABI collision validation for uniqueness. */
    if(abi->start_instance_slot==UINT32_MAX || abi->start_instance_slot>=abi->vertex_count)
        fail("multiview-witness-start-instance-slot",-1);
#endif
    if(abi->start_instance_slot!=UINT32_MAX && abi->start_instance_slot>=abi->vertex_count)
        fail("multiview-witness-start-instance-range",-1);
    uint32_t views[PS5VK_MULTIVIEW_WITNESS_VIEWS],view_count=0;
    if(ps5vk_native_view_expand(PS5VK_MULTIVIEW_WITNESS_MASK,views,
        PS5VK_MULTIVIEW_WITNESS_VIEWS,&view_count)!=VK_SUCCESS ||
       view_count!=PS5VK_MULTIVIEW_WITNESS_VIEWS)fail("multiview-witness-expansion",-1);
    for(uint32_t v=0;v<view_count;++v)if(views[v]!=v)fail("multiview-witness-view-order",-1);
    ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT];
    if(ps5_color_select_runtime_defaults(defaults,sceAgcGetRegisterDefaults()))fail("multiview-witness-defaults",-1);
    uint32_t color_bases[PS5VK_MULTIVIEW_WITNESS_VIEWS],depth_bases[PS5VK_MULTIVIEW_WITNESS_VIEWS];
    for(uint32_t v=0;v<view_count;++v) {
        struct ps5vk_target_registers color_target,depth_target;
        if(ps5vk_native_view_layer_target(d,color_view,views[v],defaults,&color_target)!=VK_SUCCESS ||
           ps5vk_native_view_layer_target(d,depth_view,views[v],NULL,&depth_target)!=VK_SUCCESS)
            fail("multiview-witness-layer-target",-1);
        color_bases[v]=color_target.registers[0].value;
        depth_bases[v]=depth_target.registers[7].value;
        if(v && (color_bases[v]<=color_bases[v-1] || depth_bases[v]<=depth_bases[v-1]))
            fail("multiview-witness-layer-order",-1);
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_MULTIVIEW_VIEW_GATE mask=%08x views=%u view_index_slot=%u start_instance_slot=%u "
        "vertex_count=%u "
        "color_first=%08x color_step=%08x depth_first=%08x depth_step=%08x",
        PS5VK_MULTIVIEW_WITNESS_MASK,view_count,abi->view_index_slot,abi->start_instance_slot,
        abi->vertex_count,
        color_bases[0],color_bases[1]-color_bases[0],depth_bases[0],depth_bases[1]-depth_bases[0]);

    /* The command buffer lives in a pool, exactly as the offline scene does: the
     * first hardware attempt was invalidated by a harness error because this
     * allocation named no pool, and the driver refused it before anything was
     * recorded. The pool owns the buffer, so destroying it after the queue is
     * idle releases both. */
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=0};
    VkCommandPool pool; CHECK(vkCreateCommandPool(d,&cpi,NULL,&pool));
    VkCommandBuffer cb=VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(vkAllocateCommandBuffers(d,&cbi,&cb));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb,&begin));
    VkClearValue clears[2]={{.color={.float32={0,0,0,1}}},{.depthStencil={1.0f,0}}};
    VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
        .framebuffer=fb,.renderArea={{0,0},{PS5VK_MULTIVIEW_WITNESS_EXTENT,PS5VK_MULTIVIEW_WITNESS_EXTENT}},
        .clearValueCount=2,.pClearValues=clears};
    vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    /* Exactly one instance, at the pinned first instance: no multi-instance draw
     * and no other value is recorded in this scene. */
    vkCmdDraw(cb,3,instance_count,0,first_instance);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_MULTIVIEW_VIEW_DRAW vertices=3 instance_count=%u first_instance=%08x",
        instance_count,first_instance);
    vkCmdEndRenderPass(cb);
    CHECK(vkEndCommandBuffer(cb));
    /* Exactly one draw: the six passes over the subpass are the backend's
     * expansion of this one command, which the gate above proved it produces. */
    if(cb->operation_count!=3)fail("multiview-witness-record",-1);
    unsigned draws=0;
    for(unsigned i=0;i<cb->operation_count;++i)
        if(cb->operations[i].type==PS5VK_DRAW)++draws;
    if(draws!=1)fail("multiview-witness-draw",-1);
    VkQueue queue; vkGetDeviceQueue(d,0,0,&queue);
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    ps5log_line(PS5LOG_MARK,"PS5VK_MULTIVIEW_VIEW_SUBMITTED draws=1 views=6 mask=0000003f");

    /* Read back through the REAL per-layer addressing: each layer's own stride,
     * every pixel classified by the shared oracle, and everything outside the
     * six rendered layers - the rest of each layer and the whole trailing guard
     * layer - folded in as guards. */
    CHECK(vkInvalidateMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
        .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=color_memory,.offset=0,.size=VK_WHOLE_SIZE}));
    CHECK(vkInvalidateMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
        .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=depth_memory,.offset=0,.size=VK_WHOLE_SIZE}));
    const uint8_t *color_bytes=color_map; const uint32_t *depth_words=depth_map;
    const uint64_t pixels=(uint64_t)PS5VK_MULTIVIEW_WITNESS_EXTENT*PS5VK_MULTIVIEW_WITNESS_EXTENT;
    const uint64_t depth_words_per_layer=depth_stride/4;
    uint8_t detiled[PS5VK_MULTIVIEW_WITNESS_EXTENT*PS5VK_MULTIVIEW_WITNESS_EXTENT*4];
    struct ps5vk_multiview_witness witness={0};
    for(uint32_t layer=0;layer<PS5VK_MULTIVIEW_WITNESS_VIEWS;++layer) {
        /* Colour is a tiled 64KB_R_X surface: its first words are NOT pixels. Every
         * layer is detiled by the driver's own arithmetic first, and only the
         * resulting 4096 linear pixels reach the oracle. */
        if(ps5vk_rgba8_64k_rx_detile(detiled,sizeof(detiled),
            color_bytes+(VkDeviceSize)layer*color_stride,(size_t)color_stride,
            PS5VK_MULTIVIEW_WITNESS_EXTENT,PS5VK_MULTIVIEW_WITNESS_EXTENT))
            fail("multiview-witness-detile",-1);
        for(uint64_t p=0;p<pixels;++p)
            ps5vk_multiview_witness_color_pixel(&witness,layer,detiled+4*p);
        /* Depth has no such equations in this driver - depth_layout.h says so -
         * and none are invented here. Because the witness stage writes ONE
         * uniform depth per view and the pass clears with a uniform dword, both
         * are tiling-invariant, so the whole footprint is COUNTED: exactly 4096
         * words of this view, no word of another view, the rest exactly cleared.
         * That is the coverage and independence proof, without Z_X coordinates. */
        const uint32_t *depth_base=depth_words+((VkDeviceSize)layer*depth_stride)/4;
        for(uint64_t w=0;w<depth_words_per_layer;++w)
            ps5vk_multiview_witness_depth(&witness,layer,depth_base[w]);
    }
    /* Only the TRAILING layer is a guard: the six the pass owns are cleared and
     * written across their whole footprint, so their padding is not untouched
     * storage and is judged by the depth counts above instead. */
    const uint32_t *color_guard=(const uint32_t *)(color_bytes+
        (VkDeviceSize)PS5VK_MULTIVIEW_WITNESS_VIEWS*color_stride);
    for(VkDeviceSize w=0;w<color_stride/4;++w)
        ps5vk_multiview_witness_guard(&witness,color_guard[w],PS5VK_MULTIVIEW_GUARD);
    const uint32_t *depth_guard=depth_words+
        (VkDeviceSize)PS5VK_MULTIVIEW_WITNESS_VIEWS*depth_stride/4;
    for(VkDeviceSize w=0;w<depth_stride/4;++w)
        ps5vk_multiview_witness_guard(&witness,depth_guard[w],PS5VK_MULTIVIEW_GUARD);
    const int verified=ps5vk_multiview_witness_verify(&witness,
        PS5VK_MULTIVIEW_WITNESS_VIEWS,depth_words_per_layer);
    for(uint32_t layer=0;layer<PS5VK_MULTIVIEW_WITNESS_VIEWS;++layer)
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_MULTIVIEW_VIEW_LAYER layer=%u view=%u pixels=%llu color_expected=%llu "
            "color_other_view=%llu color_other=%llu depth_expected=%llu depth_other=%llu "
            "depth_remainder_clear=%llu depth_remainder_unknown=%llu "
            "color_first_foreign=%02x%02x%02x%02x color_foreign_views=%02x color_foreign_view=%u "
            "depth_foreign_views=%02x depth_foreign_view=%u instance_failed=%llu",
            layer,ps5vk_multiview_witness_view(layer),(unsigned long long)witness.layer[layer].pixels,
            (unsigned long long)witness.layer[layer].expected,
            (unsigned long long)witness.layer[layer].other_view,
            (unsigned long long)witness.layer[layer].other,
            (unsigned long long)witness.layer[layer].depth_expected,
            (unsigned long long)witness.layer[layer].depth_other,
            (unsigned long long)witness.layer[layer].depth_clear,
            (unsigned long long)witness.layer[layer].depth_unknown,
            witness.layer[layer].color_first_foreign[0],witness.layer[layer].color_first_foreign[1],
            witness.layer[layer].color_first_foreign[2],witness.layer[layer].color_first_foreign[3],
            witness.layer[layer].color_foreign_mask,witness.layer[layer].color_foreign_view,
            witness.layer[layer].depth_foreign_mask,witness.layer[layer].depth_foreign_view,
            (unsigned long long)witness.layer[layer].instance_failed);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_MULTIVIEW_VIEW_PROBE views=%u mask=%08x framebuffer_layers=1 extent=%u layers_per_image=%u "
        "color=detiled depth=footprint_count load_op=dont_care depth_words_per_layer=%llu guard_layer=%u "
        "guard_words=%llu guard_mismatches=%llu instance=%08x instance_count=%u instance_witness=%d strict_verified=%d",
        PS5VK_MULTIVIEW_WITNESS_VIEWS,PS5VK_MULTIVIEW_WITNESS_MASK,PS5VK_MULTIVIEW_WITNESS_EXTENT,
        PS5VK_MULTIVIEW_WITNESS_LAYERS,(unsigned long long)depth_words_per_layer,
        PS5VK_MULTIVIEW_WITNESS_VIEWS,(unsigned long long)witness.guard_words,
        (unsigned long long)witness.guard_mismatches,first_instance,instance_count,
#if PS5VK_MULTIVIEW_INSTANCE_PROBE
        1,
#else
        0,
#endif
        verified);
    if(!verified)fail("multiview-witness-verdict",-1);

    /* The queue is idle and the readback is done: the pool releases the command
     * buffer it owns here, before any device teardown. */
    vkDestroyCommandPool(d,pool,NULL);
    vkDestroyPipeline(d,pipeline,NULL);
    vkDestroyPipelineLayout(d,layout,NULL);
    vkDestroyShaderModule(d,shaders[0],NULL); vkDestroyShaderModule(d,shaders[1],NULL);
    vkDestroyFramebuffer(d,fb,NULL); vkDestroyRenderPass(d,pass,NULL);
    vkDestroyImageView(d,color_view,NULL); vkDestroyImageView(d,depth_view,NULL);
    vkUnmapMemory(d,color_memory); vkUnmapMemory(d,depth_memory);
    vkDestroyImage(d,color_image,NULL); vkDestroyImage(d,depth_image,NULL);
    vkFreeMemory(d,color_memory,NULL); vkFreeMemory(d,depth_memory,NULL);
}
#endif
#if defined(PS5VK_CLIP_CULL_PROBE) && PS5VK_CLIP_CULL_PROBE
/* T04-D1: the packed clip/cull distance witness.
 *
 * One triangle pair covers the target with a varying affine in the position, and
 * the pre-raster stage's clip/cull distances are built from the same position,
 * so every pixel has one predicted answer: the clip cases keep exactly the half
 * or quadrant their planes leave (interpolated as if the primitive had been
 * cut), and the cull cases must leave the target at its clear colour because one
 * negative vertex discards the whole primitive. The seven cases share one vertex
 * module and differ only in the specialization constant that selects the
 * distances, so the exported masks and linked state are identical across them.
 *
 * The readback is judged by src/clip_cull_witness.c - the same oracle the host
 * regression drives - and the run fails closed when a case does not verify. */
enum { PS5VK_CLIP_CULL_EXTENT = 64 };
#define PS5VK_CLIP_CULL_GUARD UINT32_C(0x5a5a5a5a)

/* The specialization constant the witness vertex stage reads. */
enum { PS5VK_CLIP_CULL_MODE_CONSTANT = 0 };

static uint64_t clip_cull_digest(const uint8_t *bytes,size_t size)
{
    uint64_t hash=UINT64_C(0xcbf29ce484222325);
    for(size_t i=0;i<size;++i) {
        hash^=bytes[i];
        hash*=UINT64_C(0x100000001b3);
    }
    return hash;
}

/* The vertex module one case renders with: the control declares no distance at
 * all, every other case selects its distances with the specialization. */
static int clip_cull_mode(unsigned witness_case,int *mode)
{
    switch(witness_case) {
    case PS5VK_CLIP_CULL_PLAIN: *mode=-1; return 1;   /* control module */
    case PS5VK_CLIP_CULL_POSITIVE: *mode=0; return 1;
    case PS5VK_CLIP_CULL_CLIP_HALF: *mode=1; return 1;
    case PS5VK_CLIP_CULL_CLIP_QUADRANT: *mode=2; return 1;
    case PS5VK_CLIP_CULL_CULL_HALF: *mode=3; return 1;
    case PS5VK_CLIP_CULL_CULL_NEGATIVE: *mode=4; return 1;
    case PS5VK_CLIP_CULL_MIXED: *mode=5; return 1;
    case PS5VK_CLIP_CULL_CULL_INDEX: *mode=6; return 1;
    case PS5VK_CLIP_CULL_DYNAMIC_INDEX: *mode=7; return 1;
    /* The same program as the direct quadrant: only the draw path differs. */
    case PS5VK_CLIP_CULL_INDIRECT_QUADRANT: *mode=2; return 1;
    /* The pixel read runs the all-positive distance set: coverage stays the
     * control's and the fragment stage multiplies its varying by clip
     * distance 0, so the image is a different function of the position. */
    case PS5VK_CLIP_CULL_PIXEL_READ: *mode=0; return 1;
    }
    return 0;
}

static void clip_cull_probe(VkDevice d)
{
    const uint32_t extent=PS5VK_CLIP_CULL_EXTENT;
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={extent,extent,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL};
    VkImage image; CHECK(vkCreateImage(d,&ii,NULL,&image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(d,image,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=req.size};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(d,&ai,NULL,&memory));
    CHECK(vkBindImageMemory(d,image,memory,0));
    void *map=NULL; CHECK(vkMapMemory(d,memory,0,VK_WHOLE_SIZE,0,&map));
    /* Every word outside the published surface stride stays a sentinel, so a
     * readback that walked past the target would be visible instead of reading
     * whatever the allocator returned. */
    for(VkDeviceSize i=0;i<req.size/4;++i)((uint32_t *)map)[i]=PS5VK_CLIP_CULL_GUARD;
    CHECK(vkFlushMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
        .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.offset=0,.size=req.size}));
    VkDeviceSize stride=0,alignment=0,bytes=0;
    if(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM,extent,extent,1,
        &stride,&alignment,&bytes)!=VK_SUCCESS)fail("clip-cull-storage",-1);
    if(!stride || bytes!=stride || stride>req.size)fail("clip-cull-stride",-1);
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageViewCreateInfo cvi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange=range};
    VkImageView view; CHECK(vkCreateImageView(d,&cvi,NULL,&view));
    /* CLEAR, not DONT_CARE: the clear colour is the oracle's "no fragment was
     * written here" value, and the witness reads it back as evidence. */
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference colorref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&colorref};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass pass; CHECK(vkCreateRenderPass(d,&ri,NULL,&pass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=pass,.attachmentCount=1,.pAttachments=&view,
        .width=extent,.height=extent,.layers=1};
    VkFramebuffer fb; CHECK(vkCreateFramebuffer(d,&fi,NULL,&fb));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d,&li,NULL,&layout));
    VkShaderModule fragment;
    VkShaderModuleCreateInfo fsi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_fragment),.pCode=ps5vk_runtime_fragment};
    CHECK(vkCreateShaderModule(d,&fsi,NULL,&fragment));
    /* The pixel end of the interface: a fragment stage that reads
     * gl_ClipDistance[0] instead of only a varying. */
    VkShaderModule pixel_read_fragment;
    VkShaderModuleCreateInfo prfsi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_clip_distance_read_fragment),
        .pCode=ps5vk_runtime_clip_distance_read_fragment};
    CHECK(vkCreateShaderModule(d,&prfsi,NULL,&pixel_read_fragment));
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=0};
    VkCommandPool pool; CHECK(vkCreateCommandPool(d,&cpi,NULL,&pool));
    VkQueue queue; vkGetDeviceQueue(d,0,0,&queue);
    static uint8_t detiled[PS5VK_CLIP_CULL_EXTENT*PS5VK_CLIP_CULL_EXTENT*4];
    uint64_t digests[PS5VK_CLIP_CULL_CASES]={0};
    /* The cross-regression against T03's indirect command path: one recorded
     * VkDrawIndirectCommand, read through the driver's own buffer object. The
     * command says exactly what every other case issues directly, so the image
     * must be the direct quadrant's image and nothing else may differ. */
    VkBuffer indirect_buffer=VK_NULL_HANDLE;
    VkDeviceMemory indirect_memory=VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=sizeof(VkDrawIndirectCommand),.usage=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
        CHECK(vkCreateBuffer(d,&bi,NULL,&indirect_buffer));
        VkMemoryRequirements req;vkGetBufferMemoryRequirements(d,indirect_buffer,&req);
        VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=req.size};
        CHECK(vkAllocateMemory(d,&mi,NULL,&indirect_memory));
        CHECK(vkBindBufferMemory(d,indirect_buffer,indirect_memory,0));
        void *mapped=NULL;
        CHECK(vkMapMemory(d,indirect_memory,0,VK_WHOLE_SIZE,0,&mapped));
        const VkDrawIndirectCommand command={.vertexCount=6,.instanceCount=1,
            .firstVertex=0,.firstInstance=0};
        memcpy(mapped,&command,sizeof(command));
        VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=indirect_memory,.size=VK_WHOLE_SIZE};
        CHECK(vkFlushMappedMemoryRanges(d,1,&flush));
        vkUnmapMemory(d,indirect_memory);
    }
    for(unsigned witness_case=0;witness_case<PS5VK_CLIP_CULL_CASES;++witness_case) {
        int mode=0;
        if(!clip_cull_mode(witness_case,&mode))fail("clip-cull-case",-1);
        const int32_t specialization=mode<0?0:mode;
        VkSpecializationMapEntry entry={.constantID=PS5VK_CLIP_CULL_MODE_CONSTANT,
            .offset=0,.size=sizeof(specialization)};
        VkSpecializationInfo spec={.mapEntryCount=1,.pMapEntries=&entry,
            .dataSize=sizeof(specialization),.pData=&specialization};
        const uint32_t *words=mode<0?ps5vk_runtime_clip_cull_control:
                                    ps5vk_runtime_clip_cull_probe;
        const size_t word_count=mode<0?sizeof(ps5vk_runtime_clip_cull_control)/4:
                                       sizeof(ps5vk_runtime_clip_cull_probe)/4;
        VkShaderModule vertex;
        VkShaderModuleCreateInfo vsi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=word_count*4,.pCode=words};
        CHECK(vkCreateShaderModule(d,&vsi,NULL,&vertex));
        VkPipelineShaderStageCreateInfo stages[2]={
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main",
             .pSpecializationInfo=mode<0?NULL:&spec},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
        if(witness_case==PS5VK_CLIP_CULL_PIXEL_READ)stages[1].module=pixel_read_fragment;
        VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
        VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkViewport viewport={0,0,(float)extent,(float)extent,0,1};
        VkRect2D scissor={{0,0},{extent,extent}};
        VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
        VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=15};
        VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount=1,.pAttachments=&blend_attachment};
        VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .layout=layout,.renderPass=pass,.stageCount=2,.pStages=stages,
            .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pRasterizationState=&raster,
            .pMultisampleState=&ms,.pViewportState=&vp,.pColorBlendState=&blend};
        VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(d,0,1,&pi,NULL,&pipeline));
        /* PRE-SUBMIT GATE. The context state the runtime will program must be
         * exactly the distance state this case is about: SPI_VS_OUT_CONFIG's
         * export count, SPI_SHADER_POS_FORMAT's packed position registers and
         * PA_CL_VS_OUT_CNTL's clip/cull enables. A pipeline that quietly
         * dropped the export cannot reach the GPU and still look right. */
        const struct ps5vk_native_graphics_pipeline *native=pipeline->graphics_state;
        if(!native || !native->pair || !native->pair->ready)fail("clip-cull-pipeline",-1);
        const struct ps5vk_runtime_shader *stage=&native->pair->runtime_vertex;
        const uint32_t expect_config=mode<0?0u:0x00000002u;
        const uint32_t expect_pos_format=mode<0?0x00000004u:0x00000044u;
        const uint32_t expect_out_cntl=mode<0?0u:0x01400f03u;
        uint32_t seen_config=0,seen_pos_format=0,seen_out_cntl=0,seen=0;
        for(unsigned i=0;i<stage->header.num_cx_registers;++i) {
            switch(stage->context[i].offset) {
            case 0x1b1u:seen_config=stage->context[i].value;++seen;break;
            case 0x1c3u:seen_pos_format=stage->context[i].value;++seen;break;
            case 0x207u:seen_out_cntl=stage->context[i].value;++seen;break;
            }
        }
        if(seen!=3u || seen_config!=expect_config || seen_pos_format!=expect_pos_format ||
           seen_out_cntl!=expect_out_cntl)fail("clip-cull-state",-1);
        /* The pixel-read case is about the pixel-input description, so the
         * fragment stage that will run must name the packed distance register
         * it reads. A pipeline that quietly dropped the description would
         * interpolate an attribute nothing mapped, and the image could still
         * look plausible. */
        if(witness_case==PS5VK_CLIP_CULL_PIXEL_READ) {
            const struct ps5vk_runtime_shader *pixel=&native->pair->runtime_fragment;
            unsigned described=0;
            for(unsigned i=0;i<pixel->header.num_input_semantics;++i)
                described+=(pixel->inputs[i]&255u)==PSBC_SEMANTIC_DISTANCE_REGISTER;
            if(described!=1u)fail("clip-cull-pixel-read",-1);
        }
        /* One command buffer per case: this profile refuses to record a second
         * time into a submitted buffer, and a fresh one per case keeps each
         * verdict attributable to its own recording. The pool owns them all and
         * releases them when the last case is judged. */
        VkCommandBuffer cb=VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        CHECK(vkAllocateCommandBuffers(d,&cbi,&cb));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cb,&begin));
        VkClearValue clear={.color={.float32={0.0f,0.0f,0.0f,1.0f}}};
        VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{extent,extent}},
            .clearValueCount=1,.pClearValues=&clear};
        vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        /* One primitive: six vertices, one instance, no first-instance offset. */
        const int indirect=witness_case==PS5VK_CLIP_CULL_INDIRECT_QUADRANT;
        if(indirect)
            vkCmdDrawIndirect(cb,indirect_buffer,0,1,sizeof(VkDrawIndirectCommand));
        else
            vkCmdDraw(cb,6,1,0,0);
        vkCmdEndRenderPass(cb);
        CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount=1,.pCommandBuffers=&cb};
        CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_CLIP_CULL_DRAW case=%u mode=%d vs_out_config=%08x pos_format=%08x "
            "vs_out_cntl=%08x vertices=6 instances=1 indirect=%d",
            witness_case,mode,seen_config,seen_pos_format,seen_out_cntl,indirect);
        /* Read back only after the queue is idle, then detile through the
         * driver's own arithmetic before any pixel is judged. */
        CHECK(vkInvalidateMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
            .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,
            .offset=0,.size=VK_WHOLE_SIZE}));
        if(ps5vk_rgba8_64k_rx_detile(detiled,sizeof(detiled),map,(size_t)stride,
            extent,extent))fail("clip-cull-detile",-1);
        struct ps5vk_clip_cull_witness witness={0};
        for(unsigned y=0;y<extent;++y)for(unsigned x=0;x<extent;++x)
            ps5vk_clip_cull_witness_pixel(&witness,witness_case,x,y,extent,
                                          detiled+4*((size_t)y*extent+x));
        const int verified=ps5vk_clip_cull_witness_verify(&witness,witness_case,extent);
        digests[witness_case]=clip_cull_digest(detiled,sizeof(detiled));
        /* The classification counters say a case failed; these say how. Every
         * sampled pixel is part of the private evidence for this run. */
        const uint8_t *corner=detiled;
        const uint8_t *center=detiled+4*((size_t)(extent/2)*extent+extent/2);
        /* Two samples whose x and y fractions differ, so a swapped or missing
         * varying component is visible instead of hidden by the diagonal. */
        const uint8_t *low_x=detiled+4*((size_t)(extent/4)*extent);
        const uint8_t *high_x=detiled+4*((size_t)(extent*3/4));
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_CLIP_CULL_CASE case=%u mode=%d pixels=%llu expected=%llu covered=%llu "
            "missing=%llu foreign=%llu wrong_color=%llu digest=%016llx verified=%d "
            "first_foreign=%02x%02x%02x%02x at=%u,%u first_wrong=%02x%02x%02x%02x at=%u,%u "
            "corner00=%02x%02x%02x%02x center=%02x%02x%02x%02x "
            "at_0_%u=%02x%02x%02x%02x at_%u_0=%02x%02x%02x%02x",
            witness_case,mode,(unsigned long long)witness.pixels,
            (unsigned long long)witness.expected_covered,
            (unsigned long long)witness.covered,(unsigned long long)witness.missing,
            (unsigned long long)witness.foreign,(unsigned long long)witness.wrong_color,
            (unsigned long long)digests[witness_case],verified,
            witness.first_foreign[0],witness.first_foreign[1],witness.first_foreign[2],
            witness.first_foreign[3],witness.first_foreign_x,witness.first_foreign_y,
            witness.first_wrong[0],witness.first_wrong[1],witness.first_wrong[2],
            witness.first_wrong[3],witness.first_wrong_x,witness.first_wrong_y,
            corner[0],corner[1],corner[2],corner[3],
            center[0],center[1],center[2],center[3],
            extent/4,low_x[0],low_x[1],low_x[2],low_x[3],
            extent*3/4,high_x[0],high_x[1],high_x[2],high_x[3]);
        if(!verified)fail("clip-cull-verdict",-1);
        vkDestroyPipeline(d,pipeline,NULL);
        vkDestroyShaderModule(d,vertex,NULL);
    }
    /* Cases that must agree pixel for pixel: the control against positive
     * distances, against a cull distance that is negative at one vertex only,
     * against the quadrant clip with both cull arrays exported, and the two
     * discarded cull cases against each other. The structurally different
     * images must all differ, so a readback that collapsed to one image - or to
     * a stale one - cannot satisfy this. */
    if(digests[PS5VK_CLIP_CULL_PLAIN]!=digests[PS5VK_CLIP_CULL_POSITIVE] ||
       digests[PS5VK_CLIP_CULL_PLAIN]!=digests[PS5VK_CLIP_CULL_CULL_HALF] ||
       digests[PS5VK_CLIP_CULL_CLIP_QUADRANT]!=digests[PS5VK_CLIP_CULL_MIXED] ||
       /* A dynamically indexed write of the same distances must be
        * indistinguishable from the statically indexed one: that is the whole
        * content of the variant the upstream family registers. */
       digests[PS5VK_CLIP_CULL_CLIP_QUADRANT]!=digests[PS5VK_CLIP_CULL_DYNAMIC_INDEX] ||
       digests[PS5VK_CLIP_CULL_CULL_NEGATIVE]!=digests[PS5VK_CLIP_CULL_CULL_INDEX])
        fail("clip-cull-digest-equality",-1);
    /* The pixel read keeps the control's coverage but not its colours: an image
     * that delivered the varying (or any other attribute) would equal the
     * positive case, so requiring the three to differ is what makes the case
     * evidence about the distance and not about coverage. */
    if(digests[PS5VK_CLIP_CULL_PIXEL_READ]==digests[PS5VK_CLIP_CULL_PLAIN] ||
       digests[PS5VK_CLIP_CULL_PIXEL_READ]==digests[PS5VK_CLIP_CULL_POSITIVE])
        fail("clip-cull-pixel-read-digest",-1);
    const unsigned distinct[4]={PS5VK_CLIP_CULL_PLAIN,PS5VK_CLIP_CULL_CLIP_HALF,
        PS5VK_CLIP_CULL_CLIP_QUADRANT,PS5VK_CLIP_CULL_CULL_NEGATIVE};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)
        if(digests[distinct[i]]==digests[distinct[j]])fail("clip-cull-digest-collision",-1);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CLIP_CULL_PROBE cases=%u extent=%u clear=%02x%02x%02x%02x "
        "clip_mask=%02x cull_mask=%02x control_mask=000000 "
        "digest_plain=%016llx digest_positive=%016llx digest_clip_half=%016llx "
        "digest_clip_quadrant=%016llx digest_cull_half=%016llx digest_cull_negative=%016llx "
        "digest_mixed=%016llx digest_cull_index=%016llx "
        "digest_dynamic_index=%016llx digest_indirect_quadrant=%016llx "
        "digest_pixel_read=%016llx strict_verified=1",
        PS5VK_CLIP_CULL_CASES,extent,ps5vk_clip_cull_clear[0],ps5vk_clip_cull_clear[1],
        ps5vk_clip_cull_clear[2],ps5vk_clip_cull_clear[3],0x03u,0x0cu,
        (unsigned long long)digests[PS5VK_CLIP_CULL_PLAIN],
        (unsigned long long)digests[PS5VK_CLIP_CULL_POSITIVE],
        (unsigned long long)digests[PS5VK_CLIP_CULL_CLIP_HALF],
        (unsigned long long)digests[PS5VK_CLIP_CULL_CLIP_QUADRANT],
        (unsigned long long)digests[PS5VK_CLIP_CULL_CULL_HALF],
        (unsigned long long)digests[PS5VK_CLIP_CULL_CULL_NEGATIVE],
        (unsigned long long)digests[PS5VK_CLIP_CULL_MIXED],
        (unsigned long long)digests[PS5VK_CLIP_CULL_CULL_INDEX],
        (unsigned long long)digests[PS5VK_CLIP_CULL_DYNAMIC_INDEX],
        (unsigned long long)digests[PS5VK_CLIP_CULL_INDIRECT_QUADRANT],
        (unsigned long long)digests[PS5VK_CLIP_CULL_PIXEL_READ]);
    vkDestroyBuffer(d,indirect_buffer,NULL);
    vkFreeMemory(d,indirect_memory,NULL);
    vkDestroyCommandPool(d,pool,NULL);
    vkDestroyShaderModule(d,fragment,NULL);
    vkDestroyShaderModule(d,pixel_read_fragment,NULL);
    vkDestroyPipelineLayout(d,layout,NULL);
    vkDestroyFramebuffer(d,fb,NULL);
    vkDestroyRenderPass(d,pass,NULL);
    vkDestroyImageView(d,view,NULL);
    vkUnmapMemory(d,memory);
    vkDestroyImage(d,image,NULL);
    vkFreeMemory(d,memory,NULL);
}
#endif
#if defined(PS5VK_GEOMETRY_PROBE) && PS5VK_GEOMETRY_PROBE
/* T04-G1: the geometry coverage witness.
 *
 * One vertex module emits the two triangles that tile the target and one
 * fragment module writes what it reads, so the only thing that can change the
 * image is the geometry stage in between: passthrough must reproduce the
 * two-stage control pixel for pixel, shrink must cover exactly the centred
 * square its scaled triangles tile, suppression must leave the target clear -
 * which no vertex stage can do - and the varying rewrite must keep the coverage
 * while changing every colour, proving the fragment stage reads what the
 * geometry stage wrote.
 *
 * The readback is judged by src/geometry_witness.c, the same oracle the host
 * regression drives, and the run fails closed when a case does not verify. */
enum { PS5VK_GEOMETRY_EXTENT = 64 };
#define PS5VK_GEOMETRY_GUARD UINT32_C(0x5a5a5a5a)
enum { PS5VK_GEOMETRY_MODE_CONSTANT = 0 };
/* The readback cases draw 21 vertices = 7 triangles instead of the witness's
 * six: item indices 0..20 then exist, which is enough for the three plausible
 * reads of gl_in[k] to be told apart - item 3p+k if the item index passed to the
 * geometry half is an item index, item 5(3p+k) if it is in dwords and item
 * 20(3p+k) if it is in bytes. They also use a pre-raster stage that gives every
 * vertex a different x, so the value the geometry half reads back names the item
 * it came from. */
enum { PS5VK_GEOMETRY_READ_VERTICES = 21 };

static uint64_t geometry_digest(const uint8_t *bytes,size_t size)
{
    uint64_t hash=UINT64_C(0xcbf29ce484222325);
    for(size_t i=0;i<size;++i) {
        hash^=bytes[i];
        hash*=UINT64_C(0x100000001b3);
    }
    return hash;
}

#if PS5VK_TESS_PROBE
/* The pre-submit receipt. Every field is READ BACK from the pipeline object
 * the draw is about to use - never a literal, never a value this harness just
 * wrote from its own copy - so the line reports the launch state the engine
 * will actually see. The predecessor's witness printed "di_patch=9" as a
 * string literal and scanned the WRONG register bank for the stage enables
 * (pair->runtime_vertex holds the domain program; the enables live in the
 * separate pair->tess_state[0]), so it reported stages_en=00000000 for a
 * pipeline whose enables were in fact programmed. Read the source of truth. */
static void tess_receipt(const char *variant,
    const struct ps5vk_native_graphics_pipeline *native,unsigned vertices)
{
    const struct ps5vk_graphics_pair *pair=native->pair;
    uint32_t tf_param=0;
    for(unsigned i=0;i<pair->runtime_hull.header.num_cx_registers;++i)
        if(pair->runtime_hull.context[i].offset==0x2db)
            tf_param=pair->runtime_hull.context[i].value;
    /* The merged LS/HS resource register the hull actually launches with.
     * LDS_SIZE lives at bits 18..26 and the compiler cannot publish it, so a
     * run that reports zero here is a hull launched without the LDS its own
     * code writes. */
    uint32_t hs_rsrc2=0;
    for(unsigned i=0;i<pair->runtime_hull.header.num_sh_registers;++i)
        if(pair->runtime_hull.shader[i].offset==0x10b)
            hs_rsrc2=pair->runtime_hull.shader[i].value;
    const uint32_t *table=(const uint32_t *)pair->tess_rings;
    const uint64_t pipeline_va=(uint64_t)(uintptr_t)native;
    const uint64_t pair_va=(uint64_t)(uintptr_t)pair;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_TESS_RECEIPT build=%s variant=%s vertices=%u no_draw=%d "
        "pipeline=%08x%08x pair=%08x%08x tessellation=%u "
        "stages_en=%08x@%03x primitive=%08x ls_hs_config=%08x@%03x "
        "tf_param=%08x hs_rsrc2=%08x lds_granules=%u ring_table=%08x%08x "
        "e5=%08x %08x %08x %08x e6=%08x %08x %08x %08x",
        PS5VK_TESS_BUILD_ID,variant,vertices,(int)PS5VK_TESS_NO_DRAW,
        (uint32_t)(pipeline_va>>32),(uint32_t)pipeline_va,
        (uint32_t)(pair_va>>32),(uint32_t)pair_va,
        (unsigned)pair->tessellation,
        pair->tess_state[0].value,(unsigned)pair->tess_state[0].offset,
        pair->uc.vgt_primitive_type.value,
        pair->tess_state[1].value,(unsigned)pair->tess_state[1].offset,
        tf_param,hs_rsrc2,(hs_rsrc2>>18)&0x1ffu,
        pair->tess_ring_table_high,pair->tess_ring_table_low,
        table[20],table[21],table[22],table[23],
        table[24],table[25],table[26],table[27]);
}
#endif

static void geometry_probe(VkDevice d)
{
    const uint32_t extent=PS5VK_GEOMETRY_EXTENT;
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={extent,extent,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL};
    VkImage image; CHECK(vkCreateImage(d,&ii,NULL,&image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(d,image,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=req.size};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(d,&ai,NULL,&memory));
    CHECK(vkBindImageMemory(d,image,memory,0));
    void *map=NULL; CHECK(vkMapMemory(d,memory,0,VK_WHOLE_SIZE,0,&map));
    for(VkDeviceSize i=0;i<req.size/4;++i)((uint32_t *)map)[i]=PS5VK_GEOMETRY_GUARD;
    CHECK(vkFlushMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
        .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.offset=0,.size=req.size}));
    VkDeviceSize stride=0,alignment=0,bytes=0;
    if(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM,extent,extent,1,
        &stride,&alignment,&bytes)!=VK_SUCCESS)fail("geometry-storage",-1);
    if(!stride || bytes!=stride || stride>req.size)fail("geometry-stride",-1);
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageViewCreateInfo cvi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange=range};
    VkImageView view; CHECK(vkCreateImageView(d,&cvi,NULL,&view));
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference colorref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&colorref};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass pass; CHECK(vkCreateRenderPass(d,&ri,NULL,&pass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=pass,.attachmentCount=1,.pAttachments=&view,
        .width=extent,.height=extent,.layers=1};
    VkFramebuffer fb; CHECK(vkCreateFramebuffer(d,&fi,NULL,&fb));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d,&li,NULL,&layout));
    VkShaderModule vertex_module,fragment_module;
    VkShaderModuleCreateInfo vsi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_vertex),.pCode=ps5vk_runtime_geometry_vertex};
    VkShaderModuleCreateInfo gsi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_stage),.pCode=ps5vk_runtime_geometry_stage};
    VkShaderModuleCreateInfo fsi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_fragment),.pCode=ps5vk_runtime_fragment};
    CHECK(vkCreateShaderModule(d,&vsi,NULL,&vertex_module));
    VkShaderModule geometry_module;
    CHECK(vkCreateShaderModule(d,&gsi,NULL,&geometry_module));
    /* The envelope's own pre-raster half: `max_vertices` is a module-level
     * declaration, so the 256-vertex emission the feature's mandatory minimum
     * names needs a module of its own. Every other case keeps the witness stage. */
    VkShaderModule envelope_module;
    VkShaderModuleCreateInfo egi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_envelope_stage),
        .pCode=ps5vk_runtime_geometry_envelope_stage};
    CHECK(vkCreateShaderModule(d,&egi,NULL,&envelope_module));
    /* The invocations case's own pre-raster half, for the same reason: the
     * invocations count is a module-level declaration too. */
    VkShaderModule components_vertex_module,components_module;
    VkShaderModuleCreateInfo components_vertex_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_components_vertex),
        .pCode=ps5vk_runtime_geometry_components_vertex};
    VkShaderModuleCreateInfo components_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_components_stage),
        .pCode=ps5vk_runtime_geometry_components_stage};
    CHECK(vkCreateShaderModule(d,&components_vertex_info,NULL,&components_vertex_module));
    CHECK(vkCreateShaderModule(d,&components_info,NULL,&components_module));
    VkShaderModule primitive_id_module;
    VkShaderModuleCreateInfo pgi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_primitive_id_stage),
        .pCode=ps5vk_runtime_geometry_primitive_id_stage};
    CHECK(vkCreateShaderModule(d,&pgi,NULL,&primitive_id_module));
    /* The component envelope's pixel half: it declares an input for every one of
     * the sixteen vec4s the geometry stage writes and folds all sixty-four into
     * the colour, so the OUTPUT side of the envelope is observable. */
    VkShaderModule components_output_fragment_module;
    VkShaderModuleCreateInfo cofi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_output_components_fragment),
        .pCode=ps5vk_runtime_geometry_output_components_fragment};
    CHECK(vkCreateShaderModule(d,&cofi,NULL,&components_output_fragment_module));
    /* The input families: their own pre-raster half, whose positions and colours
     * identify the vertex, plus the point-list and line-list geometry stages that
     * read their input primitive's own arity. The pipeline's input assembly is
     * the family's topology, which is what the interface policy binds the
     * stage's declared input array to. */
    VkShaderModule family_vertex_module,points_module,lines_module;
    VkShaderModuleCreateInfo fvi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_family_vertex),
        .pCode=ps5vk_runtime_geometry_family_vertex};
    VkShaderModuleCreateInfo poi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_points_stage),
        .pCode=ps5vk_runtime_geometry_points_stage};
    VkShaderModuleCreateInfo loi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_lines_stage),
        .pCode=ps5vk_runtime_geometry_lines_stage};
    CHECK(vkCreateShaderModule(d,&fvi,NULL,&family_vertex_module));
    CHECK(vkCreateShaderModule(d,&poi,NULL,&points_module));
    CHECK(vkCreateShaderModule(d,&loi,NULL,&lines_module));
#if PS5VK_GEOMETRY_ORDER_PROBE
    /* Primitive-restart witness (report-only, order-probe payloads only). The
     * pinned CTS geometry builder enables primitive restart for strip
     * topologies; this pair draws two quads with a gap as one indexed strip
     * whose index list carries a restart index between them, so a cut draws two
     * quads and a missing cut threads a bridging primitive across the gap. The
     * control below is the same draw with restart disabled. */
    VkShaderModule restart_vertex_module,restart_fragment_module;
    VkShaderModuleCreateInfo rvi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_primitive_restart_vertex),
        .pCode=ps5vk_runtime_primitive_restart_vertex};
    VkShaderModuleCreateInfo rfi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_primitive_restart_fragment),
        .pCode=ps5vk_runtime_primitive_restart_fragment};
    CHECK(vkCreateShaderModule(d,&rvi,NULL,&restart_vertex_module));
    CHECK(vkCreateShaderModule(d,&rfi,NULL,&restart_fragment_module));
    VkBuffer restart_vertex_buffer=VK_NULL_HANDLE,restart_index_buffer=VK_NULL_HANDLE;
    VkDeviceMemory restart_vertex_memory=VK_NULL_HANDLE,restart_index_memory=VK_NULL_HANDLE;
    {
        VkBufferCreateInfo vbi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64,
            .usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        CHECK(vkCreateBuffer(d,&vbi,NULL,&restart_vertex_buffer));
        VkMemoryRequirements vr;vkGetBufferMemoryRequirements(d,restart_vertex_buffer,&vr);
        VkMemoryAllocateInfo va={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=vr.size+vr.alignment};
        CHECK(vkAllocateMemory(d,&va,NULL,&restart_vertex_memory));
        CHECK(vkBindBufferMemory(d,restart_vertex_buffer,restart_vertex_memory,vr.alignment));
        void *vertices;CHECK(vkMapMemory(d,restart_vertex_memory,0,VK_WHOLE_SIZE,0,&vertices));
        memset(vertices,0,(size_t)va.allocationSize);
        const float quads[8][2]={{-0.9f,-0.5f},{-0.2f,-0.5f},{-0.9f,0.5f},{-0.2f,0.5f},
                                 { 0.2f,-0.5f},{ 0.9f,-0.5f},{ 0.2f,0.5f},{ 0.9f,0.5f}};
        memcpy((unsigned char *)vertices+vr.alignment,quads,sizeof(quads));
        VkMappedMemoryRange vflush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=restart_vertex_memory,.size=VK_WHOLE_SIZE};
        vkFlushMappedMemoryRanges(d,1,&vflush);
        vkUnmapMemory(d,restart_vertex_memory);
        VkBufferCreateInfo ibi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=32,
            .usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        CHECK(vkCreateBuffer(d,&ibi,NULL,&restart_index_buffer));
        VkMemoryRequirements ir;vkGetBufferMemoryRequirements(d,restart_index_buffer,&ir);
        VkMemoryAllocateInfo ia={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=ir.size+ir.alignment};
        CHECK(vkAllocateMemory(d,&ia,NULL,&restart_index_memory));
        CHECK(vkBindBufferMemory(d,restart_index_buffer,restart_index_memory,ir.alignment));
        void *indices;CHECK(vkMapMemory(d,restart_index_memory,0,VK_WHOLE_SIZE,0,&indices));
        memset(indices,0,(size_t)ia.allocationSize);
        const uint16_t list[9]={0,1,2,3,0xffff,4,5,6,7};
        memcpy((unsigned char *)indices+ir.alignment,list,sizeof(list));
        VkMappedMemoryRange iflush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=restart_index_memory,.size=VK_WHOLE_SIZE};
        vkFlushMappedMemoryRanges(d,1,&iflush);
        vkUnmapMemory(d,restart_index_memory);
    }
#endif
    VkShaderModule invocations_module;
    VkShaderModuleCreateInfo igi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_invocations_stage),
        .pCode=ps5vk_runtime_geometry_invocations_stage};
    CHECK(vkCreateShaderModule(d,&igi,NULL,&invocations_module));
    CHECK(vkCreateShaderModule(d,&fsi,NULL,&fragment_module));
    /* The readback's own pre-raster half: a position whose x is unique per vertex
     * index, so the bytes the geometry half reads identify the item they came
     * from. Every other case keeps the witness's two-triangle vertex stage. */
    VkShaderModule identity_vertex_module;
    VkShaderModuleCreateInfo ivsi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_identity_vertex),
        .pCode=ps5vk_runtime_geometry_identity_vertex};
    CHECK(vkCreateShaderModule(d,&ivsi,NULL,&identity_vertex_module));
    /* Synthetic suppress diagnostic: a fragment stage that reads no input, so
     * the geometry half's suppress case (which emits nothing) still forms a
     * legal pipeline under this profile's draw-ABI rule instead of being
     * refused for an input the pre-raster stage never exports. Every other case
     * keeps fragment_module. */
    VkShaderModule suppress_fragment_module;
    VkShaderModuleCreateInfo sfsi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(ps5vk_runtime_geometry_suppress_fragment),
        .pCode=ps5vk_runtime_geometry_suppress_fragment};
    CHECK(vkCreateShaderModule(d,&sfsi,NULL,&suppress_fragment_module));
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=0};
    VkCommandPool pool; CHECK(vkCreateCommandPool(d,&cpi,NULL,&pool));
    VkQueue queue; vkGetDeviceQueue(d,0,0,&queue);
    static uint8_t detiled[PS5VK_GEOMETRY_EXTENT*PS5VK_GEOMETRY_EXTENT*4];
    uint64_t digests[PS5VK_GEOMETRY_CASES]={0};
#if PS5VK_GEOMETRY_ORDER_PROBE
    /* Only the diagnostic table mode keeps going after a failing verdict, so
     * only it counts failures; the shipping run fails at the first one. */
    unsigned failed_cases=0;
#endif
    /* The input-independent case runs second: if the stage never emits, the log
     * separates that from a broken ES/GS handshake before the other cases. Every
     * case appears exactly once - a short initialiser would leave the remaining
     * slots zero, i.e. extra control runs, and would silently drop the cases it
     * omitted. The sentinel runs third, before any case that can lose the
     * device, so the value oracle reports its own outcome instead of being
     * preempted. */
#if PS5VK_GEOMETRY_ORDER_PROBE
    /* Primitive-restart witness. Two quads with a gap, one indexed triangle
     * strip whose index list carries a restart index between them: a cut draws
     * exactly the two quads (nothing in the gap), a missing cut threads a
     * bridging primitive across it. The control is the same draw with restart
     * disabled, which is the state this profile accepts today, so the pair also
     * proves the witness discriminates rather than restating the state. */
    for (unsigned restart = 1; restart <= 2; ++restart) {
        const VkBool32 enable = restart == 1 ? VK_TRUE : VK_FALSE;
        VkVertexInputBindingDescription rb={.binding=0,.stride=8,
            .inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription ra={.location=0,.binding=0,
            .format=VK_FORMAT_R32G32_SFLOAT,.offset=0};
        VkPipelineVertexInputStateCreateInfo rvi_state={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&rb,
            .vertexAttributeDescriptionCount=1,.pVertexAttributeDescriptions=&ra};
        VkPipelineShaderStageCreateInfo rvs[2]={
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=restart_vertex_module,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=restart_fragment_module,.pName="main"}};
        VkPipelineInputAssemblyStateCreateInfo ria={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,.primitiveRestartEnable=enable};
        VkPipelineRasterizationStateCreateInfo rraster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
        VkPipelineMultisampleStateCreateInfo rms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkViewport rvp={0,0,(float)extent,(float)extent,0,1};
        VkRect2D rsc={{0,0},{extent,extent}};
        VkPipelineViewportStateCreateInfo rvps={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&rvp,.scissorCount=1,.pScissors=&rsc};
        VkPipelineColorBlendAttachmentState rba={.colorWriteMask=15};
        VkPipelineColorBlendStateCreateInfo rbs={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount=1,.pAttachments=&rba};
        VkGraphicsPipelineCreateInfo rpi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .layout=layout,.renderPass=pass,.stageCount=2,.pStages=rvs,
            .pVertexInputState=&rvi_state,.pInputAssemblyState=&ria,
            .pRasterizationState=&rraster,.pMultisampleState=&rms,
            .pViewportState=&rvps,.pColorBlendState=&rbs};
        VkPipeline rpipeline=VK_NULL_HANDLE;
        VkResult rrc=vkCreateGraphicsPipelines(d,0,1,&rpi,NULL,&rpipeline);
        if(rrc!=VK_SUCCESS || !rpipeline) {
            ps5log_printf(PS5LOG_MARK,"PS5VK_RESTART_PROBE restart=%u rc=%d created=0",
                (unsigned)enable,(int)rrc);
            continue;
        }
        VkCommandBuffer rcb=VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo rcbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        CHECK(vkAllocateCommandBuffers(d,&rcbi,&rcb));
        VkCommandBufferBeginInfo rbegin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(rcb,&rbegin));
        VkClearValue rclear={.color={.float32={0.0f,0.0f,0.0f,1.0f}}};
        VkRenderPassBeginInfo rrbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{extent,extent}},
            .clearValueCount=1,.pClearValues=&rclear};
        vkCmdBeginRenderPass(rcb,&rrbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(rcb,VK_PIPELINE_BIND_POINT_GRAPHICS,rpipeline);
        /* Bound at buffer offset zero: the memory alignment above is where the
         * data was written, not an offset inside the buffer. */
        VkDeviceSize restart_zero=0;
        vkCmdBindVertexBuffers(rcb,0,1,&restart_vertex_buffer,&restart_zero);
        vkCmdBindIndexBuffer(rcb,restart_index_buffer,restart_zero,VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(rcb,9,1,0,0,0);
        vkCmdEndRenderPass(rcb);
        CHECK(vkEndCommandBuffer(rcb));
        VkSubmitInfo rsubmit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&rcb};
        VkResult src_rc=vkQueueSubmit(queue,1,&rsubmit,VK_NULL_HANDLE);
        if(src_rc==VK_SUCCESS)src_rc=vkQueueWaitIdle(queue);
        if(src_rc!=VK_SUCCESS) {
            ps5log_printf(PS5LOG_MARK,"PS5VK_RESTART_PROBE restart=%u rc=%d created=1 submit_rc=%d",
                (unsigned)enable,(int)rrc,(int)src_rc);
            vkDestroyPipeline(d,rpipeline,NULL);
            continue;
        }
        CHECK(vkInvalidateMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
            .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,
            .offset=0,.size=VK_WHOLE_SIZE}));
        CHECK(ps5vk_rgba8_64k_rx_detile(detiled,sizeof(detiled),map,(size_t)stride,
            extent,extent));
        /* The oracle: the two axis-aligned quads are the coverage; any ink
         * outside them - most visibly in the gap between them - is the bridging
         * primitive a missing cut produces. */
        uint64_t expected=0,ink=0,foreign=0;
        for(unsigned y=0;y<extent;++y)for(unsigned x=0;x<extent;++x) {
            const double nx=2.0*((double)x+0.5)/(double)extent-1.0;
            const double ny=2.0*((double)y+0.5)/(double)extent-1.0;
            const int covered=(nx>=-0.9 && nx<=-0.2 && ny>=-0.5 && ny<=0.5) ||
                              (nx>= 0.2 && nx<= 0.9 && ny>=-0.5 && ny<=0.5);
            const uint8_t *px=detiled+4*((size_t)y*extent+x);
            const int is_ink=px[0]||px[1]||px[2];
            if(covered)++expected;
            if(is_ink)++ink;
            if(is_ink && !covered)++foreign;
        }
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_RESTART_PROBE restart=%u rc=%d created=1 expected=%llu ink=%llu foreign=%llu "
            "digest=%016llx gap_left=%02x%02x%02x%02x gap_right=%02x%02x%02x%02x",
            (unsigned)enable,(int)rrc,(unsigned long long)expected,(unsigned long long)ink,
            (unsigned long long)foreign,
            (unsigned long long)geometry_digest(detiled,sizeof(detiled)),
            detiled[4*((size_t)32*extent+(extent/2-4))],detiled[4*((size_t)32*extent+(extent/2-4))+1],
            detiled[4*((size_t)32*extent+(extent/2-4))+2],detiled[4*((size_t)32*extent+(extent/2-4))+3],
            detiled[4*((size_t)32*extent+(extent/2+4))],detiled[4*((size_t)32*extent+(extent/2+4))+1],
            detiled[4*((size_t)32*extent+(extent/2+4))+2],detiled[4*((size_t)32*extent+(extent/2+4))+3]);
        vkDestroyPipeline(d,rpipeline,NULL);
    }
#endif
    static const unsigned order[PS5VK_GEOMETRY_CASES]={
        PS5VK_GEOMETRY_CONTROL,PS5VK_GEOMETRY_CONSTANT,PS5VK_GEOMETRY_SENTINEL,
        PS5VK_GEOMETRY_PASSTHROUGH,PS5VK_GEOMETRY_SHRINK,PS5VK_GEOMETRY_SUPPRESS,
        PS5VK_GEOMETRY_RECOLOR,PS5VK_GEOMETRY_AMPLIFY,PS5VK_GEOMETRY_INDEXED_MARKER,
        PS5VK_GEOMETRY_READ_V0,PS5VK_GEOMETRY_READ_V1,PS5VK_GEOMETRY_READ_V2,
        PS5VK_GEOMETRY_ENVELOPE,PS5VK_GEOMETRY_INVOCATIONS,PS5VK_GEOMETRY_COMPONENTS,
        PS5VK_GEOMETRY_PRIMITIVE_ID,PS5VK_GEOMETRY_POINTS,PS5VK_GEOMETRY_LINES,
        PS5VK_GEOMETRY_POSITIONS};
    /* DIAGNOSTIC, default off: skip the geometry cases so the tessellation
     * draw is the FIRST and ONLY draw the process submits.
     *
     * Every tessellation measurement in this task has been taken after
     * nineteen geometry draws in the same process, and the handoff that
     * started this work was specifically about eliminating contamination of
     * shared state. Whether those draws leave the geometry engine in a
     * configuration a patch draw cannot recover from has never been tested -
     * it is not a register this driver writes, so no register experiment
     * could have shown it, and fifteen of those have now died. */
    const unsigned geometry_case_limit=
#if defined(PS5VK_TESS_ONLY) && PS5VK_TESS_ONLY
        0u;
#else
        PS5VK_GEOMETRY_CASES;
#endif
    for(unsigned case_index=0;case_index<geometry_case_limit;++case_index) {
        const unsigned witness_case=order[case_index];
        const int mode=ps5vk_geometry_witness_mode(witness_case);
        if(mode==-2)fail("geometry-case",-1);
        const int32_t specialization=mode<0?0:mode;
        VkSpecializationMapEntry entry={.constantID=PS5VK_GEOMETRY_MODE_CONSTANT,
            .offset=0,.size=sizeof(specialization)};
        VkSpecializationInfo spec={.mapEntryCount=1,.pMapEntries=&entry,
            .dataSize=sizeof(specialization),.pData=&specialization};
        VkPipelineShaderStageCreateInfo stages[3]={
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex_module,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_GEOMETRY_BIT,.module=geometry_module,.pName="main",
             .pSpecializationInfo=&spec},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment_module,.pName="main"}};
        if(witness_case==PS5VK_GEOMETRY_SUPPRESS)
            stages[2].module=suppress_fragment_module;
        if(witness_case>=PS5VK_GEOMETRY_READ_V0 && witness_case<=PS5VK_GEOMETRY_READ_V2)
            stages[0].module=identity_vertex_module;
        if(witness_case==PS5VK_GEOMETRY_ENVELOPE)
            stages[1].module=envelope_module;
        if(witness_case==PS5VK_GEOMETRY_INVOCATIONS)
            stages[1].module=invocations_module;
        if(witness_case==PS5VK_GEOMETRY_PRIMITIVE_ID)
            stages[1].module=primitive_id_module;
        if(witness_case==PS5VK_GEOMETRY_COMPONENTS) {
            stages[0].module=components_vertex_module;
            stages[1].module=components_module;
            /* The pixel half reads all sixteen output locations the geometry
             * stage writes, so the declared output envelope is also consumed. */
            stages[2].module=components_output_fragment_module;
        }
        /* The input families: their own pre-raster half identifies each input
         * item by position and colour, and the pipeline's input assembly carries
         * the family's topology, so the stage's declared input arity and the
         * primitive the linker programs are the same claim. */
        if(witness_case==PS5VK_GEOMETRY_POINTS || witness_case==PS5VK_GEOMETRY_LINES) {
            stages[0].module=family_vertex_module;
            stages[1].module=witness_case==PS5VK_GEOMETRY_POINTS?points_module:lines_module;
        }
        VkPipelineShaderStageCreateInfo two_stage[2]={stages[0],stages[2]};
        VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        if(witness_case==PS5VK_GEOMETRY_POINTS)
            ia.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        else if(witness_case==PS5VK_GEOMETRY_LINES)
            ia.topology=VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
        VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkViewport viewport={0,0,(float)extent,(float)extent,0,1};
        VkRect2D scissor={{0,0},{extent,extent}};
        VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
        VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=15};
        VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount=1,.pAttachments=&blend_attachment};
        VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .layout=layout,.renderPass=pass,.stageCount=mode<0?2:3,
            .pStages=mode<0?two_stage:stages,
            .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pRasterizationState=&raster,
            .pMultisampleState=&ms,.pViewportState=&vp,.pColorBlendState=&blend};
        VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(d,0,1,&pi,NULL,&pipeline));
        /* PRE-SUBMIT GATE. A geometry case must carry the merged pre-raster
         * program's pipeline state: the output topology it strips, the maximum
         * vertices it may emit, and the subgroup, on-chip, ring and max-output
         * registers the GE reads. A pipeline that lost one of them would run
         * with whatever the previous pipeline left behind. */
        const struct ps5vk_native_graphics_pipeline *native=pipeline->graphics_state;
        if(!native || !native->pair || !native->pair->ready)fail("geometry-pipeline",-1);
        const struct ps5vk_runtime_shader *stage=&native->pair->runtime_vertex;
        uint32_t topology=0,max_vertices=0,seen=0,gs_instances=0;
        if(mode>=0) {
            static const unsigned required[]={0x1ffu,0x291u,0x2abu,0x2ceu,0x2d3u};
            for(unsigned i=0;i<stage->header.num_cx_registers;++i) {
                const uint32_t offset=stage->context[i].offset;
                for(unsigned r=0;r<sizeof(required)/sizeof(required[0]);++r)
                    if(offset==required[r])++seen;
                /* VGT_GS_INSTANCE_CNT: the invocation count the GE is told to run.
                 * The invocations case is only measuring what it claims if this
                 * says 32 (enabled, count in bits 8:2). */
                if(offset==0x2e4u)
                    gs_instances=(stage->context[i].value&1u)?
                        ((stage->context[i].value>>2)&0x7fu):0u;
                if(offset==0x29bu)topology=stage->context[i].value;
                if(offset==0x2ceu)max_vertices=stage->context[i].value;
            }
            /* The envelope case declares 256 output vertices - the mandatory
             * minimum the feature names - and every other geometry case declares
             * the witness's nine, except the two input families, whose stage
             * emits a four-vertex marker per input primitive. The state the GE
             * reads must be that number, or the case would not be measuring what
             * it claims. */
            const uint32_t expect_vertices=
                witness_case==PS5VK_GEOMETRY_ENVELOPE?256u:
                ((witness_case==PS5VK_GEOMETRY_POINTS ||
                  witness_case==PS5VK_GEOMETRY_LINES)?4u:9u);
            if(seen!=sizeof(required)/sizeof(required[0]) || topology!=2u ||
               max_vertices!=expect_vertices)
                fail("geometry-state",-1);
            if(witness_case==PS5VK_GEOMETRY_INVOCATIONS && gs_instances!=32u)
                fail("geometry-invocations",-1);
        }
        VkCommandBuffer cb=VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        CHECK(vkAllocateCommandBuffers(d,&cbi,&cb));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cb,&begin));
        VkClearValue clear={.color={.float32={0.0f,0.0f,0.0f,1.0f}}};
        VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{extent,extent}},
            .clearValueCount=1,.pClearValues=&clear};
        vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        const uint32_t draw_vertices=
            (witness_case>=PS5VK_GEOMETRY_READ_V0 && witness_case<=PS5VK_GEOMETRY_READ_V2)
                ? (uint32_t)PS5VK_GEOMETRY_READ_VERTICES
                : ((witness_case==PS5VK_GEOMETRY_POINTS) ? 4u :
                   (witness_case==PS5VK_GEOMETRY_LINES) ? (uint32_t)PS5VK_GEOMETRY_FAMILY_VERTICES : 6u);
        vkCmdDraw(cb,draw_vertices,1,0,0);
        vkCmdEndRenderPass(cb);
        CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount=1,.pCommandBuffers=&cb};
        CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_GEOMETRY_DRAW case=%u mode=%d stages=%u out_prim_type=%u max_vertices=%u "
            "vertices=%u instances=1 gs_invocations=%u in_prim=%u",
            witness_case,mode,mode<0?2u:3u,topology,max_vertices,draw_vertices,
            mode<0?0u:gs_instances,(uint32_t)ia.topology);
        CHECK(vkInvalidateMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
            .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,
            .offset=0,.size=VK_WHOLE_SIZE}));
        if(ps5vk_rgba8_64k_rx_detile(detiled,sizeof(detiled),map,(size_t)stride,
            extent,extent))fail("geometry-detile",-1);
        struct ps5vk_geometry_witness witness={0};
        for(unsigned y=0;y<extent;++y)for(unsigned x=0;x<extent;++x)
            ps5vk_geometry_witness_pixel(&witness,witness_case,x,y,extent,
                                         detiled+4*((size_t)y*extent+x));
        const int verified=ps5vk_geometry_witness_verify(&witness,witness_case,extent);
        digests[witness_case]=geometry_digest(detiled,sizeof(detiled));
        const uint8_t *corner=detiled;
        const uint8_t *center=detiled+4*((size_t)(extent/2)*extent+extent/2);
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_GEOMETRY_CASE case=%u mode=%d pixels=%llu expected=%llu covered=%llu "
            "missing=%llu foreign=%llu wrong_color=%llu digest=%016llx verified=%d "
            "first_foreign=%02x%02x%02x%02x at=%u,%u first_wrong=%02x%02x%02x%02x at=%u,%u "
            "corner00=%02x%02x%02x%02x center=%02x%02x%02x%02x",
            witness_case,mode,(unsigned long long)witness.pixels,
            (unsigned long long)witness.expected_covered,
            (unsigned long long)witness.covered,(unsigned long long)witness.missing,
            (unsigned long long)witness.foreign,(unsigned long long)witness.wrong_color,
            (unsigned long long)digests[witness_case],verified,
            witness.first_foreign[0],witness.first_foreign[1],witness.first_foreign[2],
            witness.first_foreign[3],witness.first_foreign_x,witness.first_foreign_y,
            witness.first_wrong[0],witness.first_wrong[1],witness.first_wrong[2],
            witness.first_wrong[3],witness.first_wrong_x,witness.first_wrong_y,
            corner[0],corner[1],corner[2],corner[3],
            center[0],center[1],center[2],center[3]);
        if(witness_case>=PS5VK_GEOMETRY_READ_V0 && witness_case<=PS5VK_GEOMETRY_READ_V2) {
            /* The readback's own probes: the centre of each quadrant the oracle
             * expects (left/right column, low/high row) plus the middle column,
             * so the log carries the bytes the stage wrote at each of them - and
             * the clear colour where the read put its quadrant elsewhere. Without
             * this the case line only says HOW MANY pixels disagreed, not which
             * bytes the geometry half actually read. */
            const uint8_t *probe[6]={detiled+4*((size_t)16*extent+8),
                detiled+4*((size_t)16*extent+32),detiled+4*((size_t)16*extent+56),
                detiled+4*((size_t)48*extent+8),detiled+4*((size_t)48*extent+32),
                detiled+4*((size_t)48*extent+56)};
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_GEOMETRY_SAMPLE case=%u left_low=%02x%02x%02x%02x "
                "middle_low=%02x%02x%02x%02x right_low=%02x%02x%02x%02x "
                "left_high=%02x%02x%02x%02x middle_high=%02x%02x%02x%02x "
                "right_high=%02x%02x%02x%02x",
                witness_case,probe[0][0],probe[0][1],probe[0][2],probe[0][3],
                probe[1][0],probe[1][1],probe[1][2],probe[1][3],
                probe[2][0],probe[2][1],probe[2][2],probe[2][3],
                probe[3][0],probe[3][1],probe[3][2],probe[3][3],
                probe[4][0],probe[4][1],probe[4][2],probe[4][3],
                probe[5][0],probe[5][1],probe[5][2],probe[5][3]);
            /* Every item's own write place, sampled in both rows: the value a
             * read returned put its quadrants at that value's place, so these
             * samples say which item each read came from instead of only how
             * many pixels disagreed. */
            for(unsigned item=0;item<PS5VK_GEOMETRY_READ_VERTICES;++item) {
                const unsigned px=ps5vk_geometry_witness_read_pixel(item,extent);
                const uint8_t *low=detiled+4*((size_t)16*extent+px);
                const uint8_t *high=detiled+4*((size_t)48*extent+px);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_GEOMETRY_PLACE case=%u item=%u x=%u low=%02x%02x%02x%02x "
                    "high=%02x%02x%02x%02x",
                    witness_case,item,px,low[0],low[1],low[2],low[3],
                    high[0],high[1],high[2],high[3]);
            }
        }
#if PS5VK_GEOMETRY_ORDER_PROBE
        if(!verified)++failed_cases;
#else
        if(!verified)fail("geometry-verdict",-1);
#endif
        vkDestroyPipeline(d,pipeline,NULL);
    }
#if PS5VK_GEOMETRY_ORDER_PROBE
    /* The diagnostic table mode reported every case; the run still fails, so a
     * diagnostic log can never read as an acceptance pass. */
    if(failed_cases)fail("geometry-verdict",-(int)failed_cases);
#endif
    /* Passthrough and the amplified image must both reproduce the control image
     * exactly (the three sub-triangles tile the input triangle), the shrunk and
     * suppressed images must differ from it, and the varying rewrite must differ
     * from the passthrough image it shares coverage with. A stale or collapsed
     * readback cannot satisfy this. */
    if(digests[PS5VK_GEOMETRY_CONTROL]!=digests[PS5VK_GEOMETRY_PASSTHROUGH] ||
       digests[PS5VK_GEOMETRY_CONTROL]!=digests[PS5VK_GEOMETRY_AMPLIFY] ||
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_SHRINK] ||
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_SUPPRESS] ||
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_CONSTANT] ||
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_POSITIONS] ||
       /* The sentinel carries the control's coverage with the colour its own
        * mapping computes, so an image equal to any of these three means the
        * value it drew did not come from the read. */
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_SENTINEL] ||
       digests[PS5VK_GEOMETRY_PASSTHROUGH]==digests[PS5VK_GEOMETRY_SENTINEL] ||
       digests[PS5VK_GEOMETRY_POSITIONS]==digests[PS5VK_GEOMETRY_SENTINEL] ||
       /* The marker diagnostic draws two small quads, so an image equal to any
        * full-coverage case means it did not run (or the read returned
        * something that made it degenerate). */
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_INDEXED_MARKER] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_INDEXED_MARKER] ||
       digests[PS5VK_GEOMETRY_SENTINEL]==digests[PS5VK_GEOMETRY_INDEXED_MARKER] ||
       /* The readbacks draw four small squares, so they too cannot coincide with
        * a full-coverage image, and the three of them read three different
        * vertices: identical images would mean the index is not applied. */
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_READ_V0] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_READ_V0] ||
       digests[PS5VK_GEOMETRY_SENTINEL]==digests[PS5VK_GEOMETRY_READ_V0] ||
       digests[PS5VK_GEOMETRY_INDEXED_MARKER]==digests[PS5VK_GEOMETRY_READ_V0] ||
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_READ_V1] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_READ_V1] ||
       digests[PS5VK_GEOMETRY_SENTINEL]==digests[PS5VK_GEOMETRY_READ_V1] ||
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_READ_V2] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_READ_V2] ||
       digests[PS5VK_GEOMETRY_SENTINEL]==digests[PS5VK_GEOMETRY_READ_V2] ||
       /* gl_in[0] of the two primitives is +1.2 and -1.2, so that readback
        * cannot look like the two that read a single sign. */
       digests[PS5VK_GEOMETRY_READ_V0]==digests[PS5VK_GEOMETRY_READ_V1] ||
       digests[PS5VK_GEOMETRY_READ_V0]==digests[PS5VK_GEOMETRY_READ_V2] ||
       /* The envelope's band is its own shape: an image equal to the control,
        * the constant quad or the shrunk square would mean the 256 vertices did
        * not produce the band this case is named after. */
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_ENVELOPE] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_ENVELOPE] ||
       digests[PS5VK_GEOMETRY_SHRINK]==digests[PS5VK_GEOMETRY_ENVELOPE] ||
       /* The invocations image is 32 coloured columns: equal to any of these
        * would mean the invocations did not place them. */
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_INVOCATIONS] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_INVOCATIONS] ||
       digests[PS5VK_GEOMETRY_ENVELOPE]==digests[PS5VK_GEOMETRY_INVOCATIONS] ||
       /* The components image is the constant quad's shape with its own colour,
        * so equal to either would mean its inputs did not reach the stage. */
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_COMPONENTS] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_COMPONENTS] ||
       /* The per-primitive id image is two coloured columns: equal to a
        * full-coverage case would mean the markers were not placed. */
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_PRIMITIVE_ID] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_PRIMITIVE_ID] ||
       /* The input families draw their own markers under their own topology, so
        * an image equal to a full-coverage case would mean the stage never ran
        * for that shape - the exact failure the old triangle-only profile could
        * not observe, because it refused the pipeline before it could draw. */
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_POINTS] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_POINTS] ||
       digests[PS5VK_GEOMETRY_SENTINEL]==digests[PS5VK_GEOMETRY_POINTS] ||
       digests[PS5VK_GEOMETRY_INDEXED_MARKER]==digests[PS5VK_GEOMETRY_POINTS] ||
       digests[PS5VK_GEOMETRY_CONTROL]==digests[PS5VK_GEOMETRY_LINES] ||
       digests[PS5VK_GEOMETRY_CONSTANT]==digests[PS5VK_GEOMETRY_LINES] ||
       digests[PS5VK_GEOMETRY_SENTINEL]==digests[PS5VK_GEOMETRY_LINES] ||
       /* The two families draw a different number of markers of different sizes
        * from different items: identical images would mean the line stage read
        * only one vertex per primitive and produced the point case's shape. */
       digests[PS5VK_GEOMETRY_POINTS]==digests[PS5VK_GEOMETRY_LINES] ||
       digests[PS5VK_GEOMETRY_PASSTHROUGH]==digests[PS5VK_GEOMETRY_RECOLOR])
    {
        /* The digest cross-check compares images the geometry cases produced,
         * so it is meaningless - and fails closed, as it should - when those
         * cases were deliberately skipped to isolate the patch draw. Skipping
         * the check is only correct because skipping the cases is itself a
         * diagnostic: the run then carries no evidence that the device is
         * healthy, which is the stated cost of isolating the variable. */
#if !(defined(PS5VK_TESS_ONLY) && PS5VK_TESS_ONLY)
        fail("geometry-digest",-1);
#endif
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_GEOMETRY_PROBE cases=%u extent=%u clear=%02x%02x%02x%02x out_prim_type=2 "
        /* The WITNESS stage's maximum vertex count, the state the per-case check
         * reads back from VGT_GS_MAX_VERT_OUT. The envelope case declares 256 and
         * carries that number in its own DRAW record, which the parser checks
         * case by case; this one is the stage every other case uses. */
        "max_vertices=9 digest_control=%016llx digest_passthrough=%016llx "
        "digest_shrink=%016llx digest_suppress=%016llx digest_recolor=%016llx "
        "digest_amplify=%016llx digest_constant=%016llx digest_positions=%016llx "
        "digest_sentinel=%016llx digest_indexed_marker=%016llx "
        "digest_read_v0=%016llx digest_read_v1=%016llx digest_read_v2=%016llx "
        "digest_envelope=%016llx "
        "digest_invocations=%016llx digest_components=%016llx "
        "digest_primitive_id=%016llx digest_points=%016llx digest_lines=%016llx "
        "strict_verified=1",
        PS5VK_GEOMETRY_CASES,extent,ps5vk_geometry_clear[0],ps5vk_geometry_clear[1],
        ps5vk_geometry_clear[2],ps5vk_geometry_clear[3],
        (unsigned long long)digests[PS5VK_GEOMETRY_CONTROL],
        (unsigned long long)digests[PS5VK_GEOMETRY_PASSTHROUGH],
        (unsigned long long)digests[PS5VK_GEOMETRY_SHRINK],
        (unsigned long long)digests[PS5VK_GEOMETRY_SUPPRESS],
        (unsigned long long)digests[PS5VK_GEOMETRY_RECOLOR],
        (unsigned long long)digests[PS5VK_GEOMETRY_AMPLIFY],
        (unsigned long long)digests[PS5VK_GEOMETRY_CONSTANT],
        (unsigned long long)digests[PS5VK_GEOMETRY_POSITIONS],
        (unsigned long long)digests[PS5VK_GEOMETRY_SENTINEL],
        (unsigned long long)digests[PS5VK_GEOMETRY_INDEXED_MARKER],
        (unsigned long long)digests[PS5VK_GEOMETRY_READ_V0],
        (unsigned long long)digests[PS5VK_GEOMETRY_READ_V1],
        (unsigned long long)digests[PS5VK_GEOMETRY_READ_V2],
        (unsigned long long)digests[PS5VK_GEOMETRY_ENVELOPE],
        (unsigned long long)digests[PS5VK_GEOMETRY_INVOCATIONS],
        (unsigned long long)digests[PS5VK_GEOMETRY_COMPONENTS],
        (unsigned long long)digests[PS5VK_GEOMETRY_PRIMITIVE_ID],
        (unsigned long long)digests[PS5VK_GEOMETRY_POINTS],
        (unsigned long long)digests[PS5VK_GEOMETRY_LINES]);
    vkDestroyCommandPool(d,pool,NULL);
    vkDestroyShaderModule(d,vertex_module,NULL);
    vkDestroyShaderModule(d,geometry_module,NULL);
    vkDestroyShaderModule(d,envelope_module,NULL);
    vkDestroyShaderModule(d,invocations_module,NULL);
    vkDestroyShaderModule(d,primitive_id_module,NULL);
    vkDestroyShaderModule(d,family_vertex_module,NULL);
    vkDestroyShaderModule(d,points_module,NULL);
    vkDestroyShaderModule(d,lines_module,NULL);
#if PS5VK_TESS_PROBE && (PS5VK_TESS_VARIANT==1 || PS5VK_TESS_VARIANT==2 || PS5VK_TESS_VARIANT>=4)
    extern unsigned ps5vk_pipeline_refusal_site(void);
    /* CONTROL A (PS5VK_TESS_VARIANT==1) and CONTROL B (==2). Exactly ONE of
     * them is compiled into an executable, and the executable draws it once:
     * a faulting draw leaves engine state that invalidates anything measured
     * after it, so two candidates never share a process.
     *
     * Both are semantically VALID tessellation pipelines, which is what makes
     * them usable as causal evidence:
     *   A - the control half writes legal nonzero levels (outer 2/2/2, inner
     *       1) and NOTHING else, and the evaluation half's position and colour
     *       are pure functions of gl_TessCoord, so no user control-half output
     *       is consumed and the off-chip read path stays out of the picture.
     *       A completing A isolates the tessellator and the domain launch from
     *       the off-chip delivery.
     *   B - the same pipeline with legal ZERO outer levels, which discards the
     *       patch, so the hull makes no factor writes and nothing rasterises.
     *
     * Interpretation, per the handoff: A ok + C fails -> the off-chip layout,
     * the ring-offset/system-SGPR delivery, item strides and offsets. B ok +
     * A fails -> generated primitive/domain launch and factor interpretation.
     * A and B both fail -> the common hull/domain launch state, the merged
     * program ABI and the VGT/GE programming. */
#if PS5VK_TESS_VARIANT==56
#define PS5VK_TESS_CONTROL_NAME "AN-push-member"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_push_member_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_delivery_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_push_member_vertex
#elif PS5VK_TESS_VARIANT==54 || PS5VK_TESS_VARIANT==55
#if PS5VK_TESS_VARIANT==54
#define PS5VK_TESS_CONTROL_NAME "AL-indexed-instance"
#else
#define PS5VK_TESS_CONTROL_NAME "AM-indexed-indirect-instance"
#endif
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_delivery_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_delivery_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_indexed_instance_vertex
#elif PS5VK_TESS_VARIANT==53
#define PS5VK_TESS_CONTROL_NAME "AK-swapped-modes"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_swapped_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_swapped_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT>=44 && PS5VK_TESS_VARIANT<=52
#define PS5VK_TESS_CONTROL_NAME "AJ-discard-matrix"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_discard_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_discard_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#define PS5VK_TESS_CONTROL_VERTICES 51u
#elif PS5VK_TESS_VARIANT>=35 && PS5VK_TESS_VARIANT<=43
#define PS5VK_TESS_CONTROL_NAME "AI-point-matrix"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_matrix_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_matrix_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==34
#define PS5VK_TESS_CONTROL_NAME "AH-evaluation-outputs"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_coord_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_output_envelope_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==33
#define PS5VK_TESS_CONTROL_NAME "AG-dynamic-distance"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_coord_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_mixed_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==32
#define PS5VK_TESS_CONTROL_NAME "AF-mixed-distance"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_coord_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_mixed_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==31
#define PS5VK_TESS_CONTROL_NAME "AE-cull-only"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_coord_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_cullonly_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==30
#define PS5VK_TESS_CONTROL_NAME "AD-total-components"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_total_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_total_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_envelope_vertex
#elif PS5VK_TESS_VARIANT==29
#define PS5VK_TESS_CONTROL_NAME "AC-level64-points"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_level64_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_level64_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==28
#define PS5VK_TESS_CONTROL_NAME "AB-joint-envelopes"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_joint_envelope_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_joint_envelope_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_joint_envelope_vertex
#elif PS5VK_TESS_VARIANT==27
#define PS5VK_TESS_CONTROL_NAME "AA-patch-envelope"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_patch_envelope_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_patch_envelope_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==26
#define PS5VK_TESS_CONTROL_NAME "Z-component-envelope"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_envelope_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_envelope_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_envelope_vertex
#elif PS5VK_TESS_VARIANT==24 || PS5VK_TESS_VARIANT==25
#if PS5VK_TESS_VARIANT==25
#define PS5VK_TESS_CONTROL_NAME "Y-indirect-instance"
#else
#define PS5VK_TESS_CONTROL_NAME "X-instance-delivery"
#endif
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_delivery_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_delivery_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_instance_vertex
#elif PS5VK_TESS_VARIANT==22 || PS5VK_TESS_VARIANT==23
#if PS5VK_TESS_VARIANT==23
#define PS5VK_TESS_CONTROL_NAME "W-indexed32-delivery"
#else
#define PS5VK_TESS_CONTROL_NAME "V-indexed-delivery"
#endif
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_delivery_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_delivery_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_indexed_vertex
#elif PS5VK_TESS_VARIANT>=18 && PS5VK_TESS_VARIANT<=21
#if PS5VK_TESS_VARIANT==18
#define PS5VK_TESS_CONTROL_NAME "R-dense-color"
#elif PS5VK_TESS_VARIANT==21
#define PS5VK_TESS_CONTROL_NAME "U-constant-blend"
#elif PS5VK_TESS_VARIANT==20
#define PS5VK_TESS_CONTROL_NAME "T-source-blend"
#else
#define PS5VK_TESS_CONTROL_NAME "S-dense-blend"
#endif
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_quad_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_dense_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==16
#define PS5VK_TESS_CONTROL_NAME "P-domain-geometry"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_quad_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_points_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==14 || PS5VK_TESS_VARIANT==15
#if PS5VK_TESS_VARIANT==15
#define PS5VK_TESS_CONTROL_NAME "O-cache-reuse"
#else
#define PS5VK_TESS_CONTROL_NAME "N-stage-specialization"
#endif
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_spec_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_spec_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_spec_vertex
#elif PS5VK_TESS_VARIANT==13 || PS5VK_TESS_VARIANT==17
#if PS5VK_TESS_VARIANT==17
#define PS5VK_TESS_CONTROL_NAME "Q-blend-overlap"
#else
#define PS5VK_TESS_CONTROL_NAME "M-quad-points"
#endif
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_quad_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_points_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==12
#define PS5VK_TESS_CONTROL_NAME "L-unused-vs-output"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_expand32_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_patch32_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_unused_output_vertex
#elif PS5VK_TESS_VARIANT==11
#define PS5VK_TESS_CONTROL_NAME "K-expand3-to32"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_expand32_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_patch32_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_patch32_vertex
#elif PS5VK_TESS_VARIANT==10
#define PS5VK_TESS_CONTROL_NAME "J-patch32-barrier"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_patch32_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_patch32_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_patch32_vertex
#define PS5VK_TESS_CONTROL_VERTICES 32u
#elif PS5VK_TESS_VARIANT==9
#define PS5VK_TESS_CONTROL_NAME "I-isoline-linear"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_isoline_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_isoline_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==8
#define PS5VK_TESS_CONTROL_NAME "H-quad-linear"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_quad_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_quad_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==7
#define PS5VK_TESS_CONTROL_NAME "G-patch-data-linear"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_patch_data_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_patch_data_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_two_patch_vertex
#define PS5VK_TESS_CONTROL_VERTICES 6u
#elif PS5VK_TESS_VARIANT==6
#define PS5VK_TESS_CONTROL_NAME "F-two-patch-linear"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_delivery_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_delivery_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_two_patch_vertex
#define PS5VK_TESS_CONTROL_VERTICES 6u
#elif PS5VK_TESS_VARIANT==5
#define PS5VK_TESS_CONTROL_NAME "E-offchip-linear"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_delivery_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_delivery_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_delivery_vertex
#elif PS5VK_TESS_VARIANT==1
#define PS5VK_TESS_CONTROL_NAME "A-tesscoord-nonzero"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_coord_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_coord_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#elif PS5VK_TESS_VARIANT==4
/* Variant D: control A's pipeline exactly, with the evaluation half replaced
 * by one that also writes to a storage buffer.
 *
 * The position and colour maths are identical, so the image oracle is the
 * same and the two variants are directly comparable. The addition answers the
 * one question no register can: a tessellation evaluation shader has NO
 * per-vertex input except gl_TessCoord, so if it executes its exports must
 * cover pixels - and control A's image is empty with no foreign pixels
 * either. That leaves "the domain never executes" and "it executes and its
 * exports are discarded", and a memory write is the only observable the stage
 * has that does not depend on rasterisation. The buffer answers three things
 * at once: whether the stage ran, how many vertices the tessellator produced,
 * and whether gl_TessCoord arrives with real values or as zeroes. */
#define PS5VK_TESS_CONTROL_NAME "D-domain-exec-witness"
#if defined(PS5VK_TESS_SPIN) && PS5VK_TESS_SPIN==2
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_spin_iso_evaluation
#elif defined(PS5VK_TESS_SPIN) && PS5VK_TESS_SPIN
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_spin_evaluation
#else
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_witness_exec_evaluation
#endif
#if defined(PS5VK_TESS_SPIN_HULL) && PS5VK_TESS_SPIN_HULL
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_spin_hull_control
#elif defined(PS5VK_TESS_HIGH_LEVELS) && PS5VK_TESS_HIGH_LEVELS
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_coord_high_control
#else
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_coord_control
#endif

#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#else
#define PS5VK_TESS_CONTROL_NAME "B-tesscoord-zero"
#define PS5VK_TESS_CONTROL_CODE ps5vk_runtime_tess_coord_zero_control
#define PS5VK_TESS_COORD_EVAL ps5vk_runtime_tess_coord_evaluation
#define PS5VK_TESS_COORD_VERT ps5vk_runtime_tess_coord_vertex
#endif
    enum { coord_stage_count=PS5VK_TESS_VARIANT==16?5:4 };
    VkShaderModule coord_modules[coord_stage_count];
    {
    const struct { const uint32_t *code; size_t bytes; } coord_codes[coord_stage_count]={
        {PS5VK_TESS_COORD_VERT,sizeof(PS5VK_TESS_COORD_VERT)},
        {PS5VK_TESS_CONTROL_CODE,sizeof(PS5VK_TESS_CONTROL_CODE)},
        {PS5VK_TESS_COORD_EVAL,sizeof(PS5VK_TESS_COORD_EVAL)},
#if PS5VK_TESS_VARIANT==34 || PS5VK_TESS_VARIANT==53
        {ps5vk_runtime_tess_output_envelope_fragment,sizeof(ps5vk_runtime_tess_output_envelope_fragment)}
#elif PS5VK_TESS_VARIANT==33
        {ps5vk_runtime_tess_dynamic_distance_fragment,sizeof(ps5vk_runtime_tess_dynamic_distance_fragment)}
#elif PS5VK_TESS_VARIANT==32
        {ps5vk_runtime_tess_mixed_fragment,sizeof(ps5vk_runtime_tess_mixed_fragment)}
#elif PS5VK_TESS_VARIANT==31
        {ps5vk_runtime_tess_cullonly_fragment,sizeof(ps5vk_runtime_tess_cullonly_fragment)}
#elif PS5VK_TESS_VARIANT>=18 && PS5VK_TESS_VARIANT<=21
        {ps5vk_runtime_tess_dense_fragment,sizeof(ps5vk_runtime_tess_dense_fragment)}
#elif PS5VK_TESS_VARIANT==17
        {ps5vk_runtime_tess_blend_fragment,sizeof(ps5vk_runtime_tess_blend_fragment)}
#else
        {ps5vk_runtime_tess_coord_fragment,sizeof(ps5vk_runtime_tess_coord_fragment)}
#endif
#if PS5VK_TESS_VARIANT==16
        ,{ps5vk_runtime_tess_points_geometry,sizeof(ps5vk_runtime_tess_points_geometry)}
#endif
        };
    for(unsigned i=0;i<coord_stage_count;++i) {
        VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=coord_codes[i].bytes,.pCode=coord_codes[i].code};
        CHECK(vkCreateShaderModule(d,&mi,NULL,&coord_modules[i]));
    }
    }
    {
        const VkShaderStageFlagBits coord_stages[coord_stage_count]={
            VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,VK_SHADER_STAGE_FRAGMENT_BIT
#if PS5VK_TESS_VARIANT==16
            ,VK_SHADER_STAGE_GEOMETRY_BIT
#endif
            };
        VkPipelineShaderStageCreateInfo coord_stage_infos[coord_stage_count];
        for(unsigned i=0;i<coord_stage_count;++i)
            coord_stage_infos[i]=(VkPipelineShaderStageCreateInfo){
                .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage=coord_stages[i],.module=coord_modules[i],.pName="main"};
#if PS5VK_TESS_VARIANT==14 || PS5VK_TESS_VARIANT==15
        float coord_spec_values[3]={0.25f,0.5f,0.75f};
        VkSpecializationMapEntry coord_spec_entry={.constantID=0,.offset=0,.size=4};
        VkSpecializationInfo coord_specs[3];
        for(unsigned s=0;s<3;++s) {
            coord_specs[s]=(VkSpecializationInfo){.mapEntryCount=1,
                .pMapEntries=&coord_spec_entry,.dataSize=4,.pData=&coord_spec_values[s]};
            coord_stage_infos[s].pSpecializationInfo=&coord_specs[s];
        }
#endif
        VkPipelineInputAssemblyStateCreateInfo coord_ia={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
        VkPipelineTessellationStateCreateInfo coord_ts={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
            .patchControlPoints=PS5VK_TESS_VARIANT==10?32:3};
        VkPipelineVertexInputStateCreateInfo coord_vi={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo coord_raster={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
        VkPipelineMultisampleStateCreateInfo coord_ms={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkViewport coord_vp_v={0,0,(float)extent,(float)extent,0,1};
        VkRect2D coord_vp_s={{0,0},{extent,extent}};
        VkPipelineViewportStateCreateInfo coord_vp={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&coord_vp_v,.scissorCount=1,.pScissors=&coord_vp_s};
        VkPipelineColorBlendAttachmentState coord_blend_a={.colorWriteMask=15};
#if PS5VK_TESS_VARIANT==17 || (PS5VK_TESS_VARIANT>=19 && PS5VK_TESS_VARIANT<=21)
        coord_blend_a.blendEnable=VK_TRUE;
        coord_blend_a.srcColorBlendFactor=coord_blend_a.srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        coord_blend_a.dstColorBlendFactor=coord_blend_a.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
#if PS5VK_TESS_VARIANT==20 || PS5VK_TESS_VARIANT==21
        coord_blend_a.dstColorBlendFactor=coord_blend_a.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
#endif
#endif
        VkPipelineColorBlendStateCreateInfo coord_blend={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount=1,.pAttachments=&coord_blend_a};
#if PS5VK_TESS_VARIANT==21
        coord_blend_a.srcColorBlendFactor=coord_blend_a.srcAlphaBlendFactor=VK_BLEND_FACTOR_CONSTANT_ALPHA;
        coord_blend.blendConstants[3]=0.25f;
#endif
        VkPipelineLayout coord_layout=layout;
#if PS5VK_TESS_VARIANT==56
        const VkPushConstantRange member_ranges[2]={
            {VK_SHADER_STAGE_VERTEX_BIT,0,16},
            {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,16,68}};
        const VkPipelineLayoutCreateInfo member_layout_info={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount=2,.pPushConstantRanges=member_ranges};
        CHECK(vkCreatePipelineLayout(d,&member_layout_info,NULL,&coord_layout));
#endif
#if PS5VK_TESS_VARIANT==4
        /* The witness storage buffer, its descriptor set and a pipeline
         * layout that names it. Host-visible and read straight back after the
         * fence: the buffer is a normal Vulkan allocation this harness owns,
         * unlike the tessellation rings, so the read needs no caveat about
         * coherency or about an address the harness had to guess. */
        VkBuffer coord_witness_buffer=VK_NULL_HANDLE;
        VkDeviceMemory coord_witness_memory=VK_NULL_HANDLE;
        void *coord_witness_mapped=NULL;
        VkDescriptorSetLayout coord_witness_set_layout=VK_NULL_HANDLE;
        VkDescriptorPool coord_witness_pool=VK_NULL_HANDLE;
        VkDescriptorSet coord_witness_set=VK_NULL_HANDLE;
        enum { PS5VK_TESS_WITNESS_BYTES=1024u };
        {
            VkBufferCreateInfo wbi={
                .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size=PS5VK_TESS_WITNESS_BYTES,
                .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
            CHECK(vkCreateBuffer(d,&wbi,NULL,&coord_witness_buffer));
            VkMemoryRequirements wreq;
            vkGetBufferMemoryRequirements(d,coord_witness_buffer,&wreq);
            VkMemoryAllocateInfo wmi={
                .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize=wreq.size};
            CHECK(vkAllocateMemory(d,&wmi,NULL,&coord_witness_memory));
            CHECK(vkBindBufferMemory(d,coord_witness_buffer,
                coord_witness_memory,0));
            CHECK(vkMapMemory(d,coord_witness_memory,0,VK_WHOLE_SIZE,0,
                &coord_witness_mapped));
            memset(coord_witness_mapped,0,PS5VK_TESS_WITNESS_BYTES);
            VkDescriptorSetLayoutBinding wb={
                .binding=0,
                .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount=1,
                .stageFlags=VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};
            VkDescriptorSetLayoutCreateInfo wsi={
                .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount=1,.pBindings=&wb};
            CHECK(vkCreateDescriptorSetLayout(d,&wsi,NULL,
                &coord_witness_set_layout));
            VkPipelineLayoutCreateInfo wli={
                .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount=1,.pSetLayouts=&coord_witness_set_layout};
            CHECK(vkCreatePipelineLayout(d,&wli,NULL,&coord_layout));
            VkDescriptorPoolSize wps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
            VkDescriptorPoolCreateInfo wpi={
                .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets=1,.poolSizeCount=1,.pPoolSizes=&wps};
            CHECK(vkCreateDescriptorPool(d,&wpi,NULL,&coord_witness_pool));
            VkDescriptorSetAllocateInfo wai={
                .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool=coord_witness_pool,.descriptorSetCount=1,
                .pSetLayouts=&coord_witness_set_layout};
            CHECK(vkAllocateDescriptorSets(d,&wai,&coord_witness_set));
            VkDescriptorBufferInfo wdb={coord_witness_buffer,0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet wwr={
                .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet=coord_witness_set,.descriptorCount=1,
                .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo=&wdb};
            vkUpdateDescriptorSets(d,1,&wwr,0,NULL);
        }
        /* THE WITNESS'S OWN CONTROL: an ordinary vertex+fragment draw that
         * writes the SAME buffer at a second counter.
         *
         * "The domain wrote nothing" and "a storage-buffer write from a
         * graphics stage does not land on this driver" produce an identical
         * reading, and the whole point of this witness is to tell apart things
         * that look alike. The control needs its own descriptor set layout
         * because the profile refuses a binding named for a stage the
         * pipeline does not carry, in both directions: a TESS_EVAL binding on
         * a pipeline without tessellation is refused exactly as a
         * tessellation binding was refused before the visibility fix.
         *
         * It cannot be the tessellation pipeline's own vertex half, which was
         * the first thing I tried: in a tessellation pipeline the vertex
         * shader is the LS end of the merged hull, and this driver has no
         * descriptor delivery for that program at all - the pre-raster ABI
         * slots come from the DOMAIN. That is a real gap, recorded, and much
         * too deep to open for a diagnostic. */
        {
            VkDescriptorSetLayoutBinding cb_b={
                .binding=0,
                .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount=1,
                .stageFlags=VK_SHADER_STAGE_VERTEX_BIT};
            VkDescriptorSetLayoutCreateInfo cb_si={
                .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount=1,.pBindings=&cb_b};
            VkDescriptorSetLayout cb_set_layout;
            CHECK(vkCreateDescriptorSetLayout(d,&cb_si,NULL,&cb_set_layout));
            VkPipelineLayoutCreateInfo cb_li={
                .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount=1,.pSetLayouts=&cb_set_layout};
            VkPipelineLayout cb_layout;
            CHECK(vkCreatePipelineLayout(d,&cb_li,NULL,&cb_layout));
            VkDescriptorPoolSize cb_ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
            VkDescriptorPoolCreateInfo cb_pi={
                .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets=1,.poolSizeCount=1,.pPoolSizes=&cb_ps};
            VkDescriptorPool cb_pool;
            CHECK(vkCreateDescriptorPool(d,&cb_pi,NULL,&cb_pool));
            VkDescriptorSetAllocateInfo cb_ai={
                .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool=cb_pool,.descriptorSetCount=1,
                .pSetLayouts=&cb_set_layout};
            VkDescriptorSet cb_set;
            CHECK(vkAllocateDescriptorSets(d,&cb_ai,&cb_set));
            VkDescriptorBufferInfo cb_db={coord_witness_buffer,0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet cb_wr={
                .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet=cb_set,.descriptorCount=1,
                .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo=&cb_db};
            vkUpdateDescriptorSets(d,1,&cb_wr,0,NULL);
            VkShaderModule cb_modules[2];
            const struct { const uint32_t *code; size_t bytes; } cb_codes[2]={
                {ps5vk_runtime_tess_witness_exec_vertex,
                 sizeof(ps5vk_runtime_tess_witness_exec_vertex)},
                {ps5vk_runtime_tess_coord_fragment,
                 sizeof(ps5vk_runtime_tess_coord_fragment)}};
            for(unsigned i=0;i<2;++i) {
                VkShaderModuleCreateInfo cmi={
                    .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                    .codeSize=cb_codes[i].bytes,.pCode=cb_codes[i].code};
                CHECK(vkCreateShaderModule(d,&cmi,NULL,&cb_modules[i]));
            }
            const VkShaderStageFlagBits cb_stages[2]={
                VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_FRAGMENT_BIT};
            VkPipelineShaderStageCreateInfo cb_stage_infos[2];
            for(unsigned i=0;i<2;++i)
                cb_stage_infos[i]=(VkPipelineShaderStageCreateInfo){
                    .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage=cb_stages[i],.module=cb_modules[i],.pName="main"};
            VkPipelineInputAssemblyStateCreateInfo cb_ia={
                .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
            VkPipelineVertexInputStateCreateInfo cb_vi={
                .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineRasterizationStateCreateInfo cb_raster={
                .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .lineWidth=1};
            VkPipelineMultisampleStateCreateInfo cb_ms={
                .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
            VkViewport cb_vp_v={0,0,(float)extent,(float)extent,0,1};
            VkRect2D cb_vp_s={{0,0},{extent,extent}};
            VkPipelineViewportStateCreateInfo cb_vp={
                .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                .viewportCount=1,.pViewports=&cb_vp_v,
                .scissorCount=1,.pScissors=&cb_vp_s};
            VkPipelineColorBlendAttachmentState cb_blend_a={.colorWriteMask=15};
            VkPipelineColorBlendStateCreateInfo cb_blend={
                .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .attachmentCount=1,.pAttachments=&cb_blend_a};
            VkGraphicsPipelineCreateInfo cb_gpi={
                .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .layout=cb_layout,.renderPass=pass,.stageCount=2,
                .pStages=cb_stage_infos,.pVertexInputState=&cb_vi,
                .pInputAssemblyState=&cb_ia,.pRasterizationState=&cb_raster,
                .pMultisampleState=&cb_ms,.pViewportState=&cb_vp,
                .pColorBlendState=&cb_blend};
            VkPipeline cb_pipeline=VK_NULL_HANDLE;
            const VkResult cb_rc=vkCreateGraphicsPipelines(d,0,1,&cb_gpi,NULL,
                &cb_pipeline);
            if(cb_rc!=VK_SUCCESS || !cb_pipeline) {
                /* Name the refusal site rather than the error code. The
                 * graphics pipeline path already records one, and every gate
                 * in this driver returns the same VK_ERROR_FEATURE_NOT_PRESENT
                 * from a different file - which has cost a window per gate
                 * while getting this control built. */
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_WITNESS_CONTROL rc=%d created=0 site=%u",
                    (int)cb_rc,ps5vk_pipeline_refusal_site());
            } else {
                VkCommandPool cb_cp;
                VkCommandPoolCreateInfo cb_cpi={
                    .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                    .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                    .queueFamilyIndex=0};
                CHECK(vkCreateCommandPool(d,&cb_cpi,NULL,&cb_cp));
                VkCommandBuffer cb_cb=VK_NULL_HANDLE;
                VkCommandBufferAllocateInfo cb_cbi={
                    .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                    .commandPool=cb_cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                    .commandBufferCount=1};
                CHECK(vkAllocateCommandBuffers(d,&cb_cbi,&cb_cb));
                VkCommandBufferBeginInfo cb_bi={
                    .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                CHECK(vkBeginCommandBuffer(cb_cb,&cb_bi));
                VkClearValue cb_clear={.color={.float32={0.0f,0.0f,0.0f,1.0f}}};
                VkRenderPassBeginInfo cb_rbi={
                    .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                    .renderPass=pass,.framebuffer=fb,
                    .renderArea={{0,0},{extent,extent}},
                    .clearValueCount=1,.pClearValues=&cb_clear};
                vkCmdBeginRenderPass(cb_cb,&cb_rbi,VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(cb_cb,VK_PIPELINE_BIND_POINT_GRAPHICS,cb_pipeline);
                vkCmdBindDescriptorSets(cb_cb,VK_PIPELINE_BIND_POINT_GRAPHICS,
                    cb_layout,0,1,&cb_set,0,NULL);
                vkCmdDraw(cb_cb,3,1,0,0);
                vkCmdEndRenderPass(cb_cb);
                CHECK(vkEndCommandBuffer(cb_cb));
                VkFence cb_fence=VK_NULL_HANDLE;
                VkFenceCreateInfo cb_fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                CHECK(vkCreateFence(d,&cb_fi,NULL,&cb_fence));
                VkSubmitInfo cb_submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount=1,.pCommandBuffers=&cb_cb};
                const VkResult cb_srq=vkQueueSubmit(queue,1,&cb_submit,cb_fence);
                const VkResult cb_wait=cb_srq==VK_SUCCESS?
                    vkWaitForFences(d,1,&cb_fence,VK_TRUE,300000000ull):cb_srq;
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_WITNESS_CONTROL rc=0 created=1 submit=%d wait=%d",
                    (int)cb_srq,(int)cb_wait);
                vkDestroyFence(d,cb_fence,NULL);
                vkDestroyCommandPool(d,cb_cp,NULL);
                vkDestroyPipeline(d,cb_pipeline,NULL);
            }
            for(unsigned i=0;i<2;++i)vkDestroyShaderModule(d,cb_modules[i],NULL);
            vkDestroyDescriptorPool(d,cb_pool,NULL);
            vkDestroyPipelineLayout(d,cb_layout,NULL);
            vkDestroyDescriptorSetLayout(d,cb_set_layout,NULL);
        }
#endif
        VkGraphicsPipelineCreateInfo coord_pi={
            .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .layout=coord_layout,.renderPass=pass,.stageCount=coord_stage_count,
            .pStages=coord_stage_infos,.pVertexInputState=&coord_vi,
            .pInputAssemblyState=&coord_ia,.pTessellationState=&coord_ts,
            .pRasterizationState=&coord_raster,.pMultisampleState=&coord_ms,
            .pViewportState=&coord_vp,.pColorBlendState=&coord_blend};
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 4
        /* Disjoint scissors make both draws necessary to complete the oracle;
         * drawing the identical full image twice would not prove both ran. */
        coord_vp_s.extent.width=extent/2;
#endif
        VkPipeline coord_pipeline;
        const VkResult coord_rc=vkCreateGraphicsPipelines(d,0,1,&coord_pi,NULL,
            &coord_pipeline);
        if(coord_rc==VK_SUCCESS && coord_pipeline) {
            const struct ps5vk_native_graphics_pipeline *coord_native=
                coord_pipeline->graphics_state;
            if(!coord_native || !coord_native->pair || !coord_native->pair->ready ||
               !coord_native->pair->tessellation)
                fail("tess-coord-pipeline",-1);
#if PS5VK_TESS_VARIANT==16
            if(!coord_native->pair->geometry_preraster)
                fail("tess-geometry-pipeline",-1);
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_GEOMETRY stages=5 shift_x=4 shift_y=-4 color=gbr");
#endif
            VkCommandPool coord_pool;
            VkCommandPoolCreateInfo coord_pci={
                .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex=0};
            CHECK(vkCreateCommandPool(d,&coord_pci,NULL,&coord_pool));
            VkCommandBuffer coord_cb=VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo coord_cbi={
                .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool=coord_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount=1};
            CHECK(vkAllocateCommandBuffers(d,&coord_cbi,&coord_cb));
            if(PS5VK_TESS_NO_DRAW) {
                /* The create-only control: the pipeline exists and the launch
                 * state is programmed. Nothing is submitted, so this is the
                 * artifact to run first after any console recovery. */
                tess_receipt(PS5VK_TESS_CONTROL_NAME,coord_native,0u);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_CONTROL variant=%s rc=%d created=1 draw_skipped=1",
                    PS5VK_TESS_CONTROL_NAME,(int)coord_rc);
                vkDestroyPipeline(d,coord_pipeline,NULL);
                vkDestroyCommandPool(d,coord_pool,NULL);
                goto coord_done;
            }
            VkPipeline coord_shared_pipeline=VK_NULL_HANDLE;
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 4
            coord_vp_s.offset.x=extent/2;
            coord_vp_s.extent.width=extent-extent/2;
#if PS5VK_TESS_VARIANT==14 || PS5VK_TESS_VARIANT==15
            coord_spec_values[0]=0.75f;
            coord_spec_values[1]=0.25f;
            coord_spec_values[2]=0.5f;
#endif
            CHECK(vkCreateGraphicsPipelines(d,0,1,&coord_pi,NULL,&coord_shared_pipeline));
#if PS5VK_TESS_VARIANT==15
            /* Reconstruct from a cache lease, then retire the original owner
             * before recording the draw. A compilation-only hit is insufficient. */
            struct ps5vk_compilation_cache *coord_cache=d->graphics_compiler_context;
            struct ps5vk_cache_stats before,after;
            if(!coord_cache)fail("tess-cache-absent",-1);
            ps5vk_compilation_cache_get_stats(coord_cache,&before);
            VkPipeline warm_pipeline=VK_NULL_HANDLE;
            CHECK(vkCreateGraphicsPipelines(d,0,1,&coord_pi,NULL,&warm_pipeline));
            ps5vk_compilation_cache_get_stats(coord_cache,&after);
            if(after.hits!=before.hits+1 || after.compiles!=before.compiles)
                fail("tess-cache-reuse",-1);
            vkDestroyPipeline(d,coord_shared_pipeline,NULL);
            coord_shared_pipeline=warm_pipeline;
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_CACHE_REUSE hits_delta=%llu compiles_delta=%llu original_destroyed=1",
                (unsigned long long)(after.hits-before.hits),
                (unsigned long long)(after.compiles-before.compiles));
#endif
            const struct ps5vk_native_graphics_pipeline *shared_native=
                coord_shared_pipeline->graphics_state;
            if(!shared_native || !shared_native->pair ||
               !coord_native->shared_rings ||
               shared_native->shared_rings!=coord_native->shared_rings ||
               shared_native->pair->tess_rings!=coord_native->pair->tess_rings)
                fail("tess-shared-storage",-1);
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_SHARED_STORAGE pipelines=2 same_storage=1 split_scissors=1");
#endif
            VkCommandBufferBeginInfo coord_begin={
                .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
#if PS5VK_TESS_VARIANT==25 || PS5VK_TESS_VARIANT==55
            /* Nonzero offset and zero-instance decoy catch offset mistakes.
             * This backend resolves arguments on the host at submission. */
#if PS5VK_TESS_VARIANT==55
            const VkDrawIndexedIndirectCommand coord_args[2]={{3,0,1,-65536,0},{3,2,1,-65536,3}};
#else
            const VkDrawIndirectCommand coord_args[2]={{3,0,0,0},{3,2,0,3}};
#endif
            VkBuffer coord_ab=VK_NULL_HANDLE;
            VkDeviceMemory coord_am=VK_NULL_HANDLE;
            VkBufferCreateInfo coord_abi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size=sizeof(coord_args),.usage=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
            CHECK(vkCreateBuffer(d,&coord_abi,NULL,&coord_ab));
            VkMemoryRequirements coord_ar;
            vkGetBufferMemoryRequirements(d,coord_ab,&coord_ar);
            VkMemoryAllocateInfo coord_ami={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize=coord_ar.size};
            CHECK(vkAllocateMemory(d,&coord_ami,NULL,&coord_am));
            CHECK(vkBindBufferMemory(d,coord_ab,coord_am,0));
            void *coord_arg_map;
            CHECK(vkMapMemory(d,coord_am,0,VK_WHOLE_SIZE,0,&coord_arg_map));
            memcpy(coord_arg_map,coord_args,sizeof(coord_args));
            VkMappedMemoryRange coord_af={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory=coord_am,.size=VK_WHOLE_SIZE};
            CHECK(vkFlushMappedMemoryRanges(d,1,&coord_af));
            vkUnmapMemory(d,coord_am);
#endif
#if PS5VK_TESS_VARIANT==22 || PS5VK_TESS_VARIANT==23 || PS5VK_TESS_VARIANT==54 || PS5VK_TESS_VARIANT==55
#if PS5VK_TESS_VARIANT!=22
            const uint32_t coord_indices[]={UINT32_MAX,UINT32_MAX,65541,65538,65543,UINT32_MAX};
            const VkIndexType coord_index_type=VK_INDEX_TYPE_UINT32;
            const int32_t coord_base_vertex=-65536;
#else
            const uint16_t coord_indices[]={0xffff,0xffff,4,1,6,0xffff};
            const VkIndexType coord_index_type=VK_INDEX_TYPE_UINT16;
            const int32_t coord_base_vertex=1;
#endif
            VkBuffer coord_ib=VK_NULL_HANDLE;
            VkDeviceMemory coord_im=VK_NULL_HANDLE;
            VkBufferCreateInfo coord_ibi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size=sizeof(coord_indices),.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
            CHECK(vkCreateBuffer(d,&coord_ibi,NULL,&coord_ib));
            VkMemoryRequirements coord_ir;
            vkGetBufferMemoryRequirements(d,coord_ib,&coord_ir);
            VkMemoryAllocateInfo coord_imi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize=coord_ir.size};
            CHECK(vkAllocateMemory(d,&coord_imi,NULL,&coord_im));
            CHECK(vkBindBufferMemory(d,coord_ib,coord_im,0));
            void *coord_ix_map;
            CHECK(vkMapMemory(d,coord_im,0,VK_WHOLE_SIZE,0,&coord_ix_map));
            memcpy(coord_ix_map,coord_indices,sizeof(coord_indices));
            VkMappedMemoryRange coord_flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory=coord_im,.size=VK_WHOLE_SIZE};
            CHECK(vkFlushMappedMemoryRanges(d,1,&coord_flush));
            vkUnmapMemory(d,coord_im);
#endif
            CHECK(vkBeginCommandBuffer(coord_cb,&coord_begin));
            VkClearValue coord_clear={.color={.float32={0.0f,0.0f,0.0f,1.0f}}};
            VkRenderPassBeginInfo coord_rbi={
                .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass=pass,.framebuffer=fb,
                .renderArea={{0,0},{extent,extent}},
                .clearValueCount=1,.pClearValues=&coord_clear};
            vkCmdBeginRenderPass(coord_cb,&coord_rbi,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(coord_cb,VK_PIPELINE_BIND_POINT_GRAPHICS,coord_pipeline);
#if PS5VK_TESS_VARIANT==56
            const float vertex_push[4]={0.125f,0.25f,0.5f,0.0f};
            struct {float hull[4][4];int32_t selected;} member_push={.selected=1};
            for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
                member_push.hull[i][j]=(float)(i+1)*0.0625f;
            vkCmdPushConstants(coord_cb,coord_layout,VK_SHADER_STAGE_VERTEX_BIT,
                0,sizeof(vertex_push),vertex_push);
            vkCmdPushConstants(coord_cb,coord_layout,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                16,sizeof(member_push),&member_push);
            ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_PUSH_MEMBER vertex=0:16 control=16:68 selected=1");
#endif
#if PS5VK_TESS_VARIANT==4
            vkCmdBindDescriptorSets(coord_cb,VK_PIPELINE_BIND_POINT_GRAPHICS,
                coord_layout,0,1,&coord_witness_set,0,NULL);
#endif
            /* ONE patch, three control points. */
#ifndef PS5VK_TESS_CONTROL_VERTICES
#define PS5VK_TESS_CONTROL_VERTICES 3u
#endif
#if PS5VK_TESS_VARIANT==54 || PS5VK_TESS_VARIANT==55
            vkCmdBindIndexBuffer(coord_cb,coord_ib,sizeof(coord_indices[0]),coord_index_type);
#if PS5VK_TESS_VARIANT==55
            (void)coord_base_vertex;
            vkCmdDrawIndexedIndirect(coord_cb,coord_ab,sizeof(VkDrawIndexedIndirectCommand),1,0);
#else
            vkCmdDrawIndexed(coord_cb,3,2,1,coord_base_vertex,3);
#endif
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_INDEXED_INSTANCE indirect=%u binding_offset=4 first_index=1 vertex_offset=-65536 instances=2 first_instance=3",
                (unsigned)(PS5VK_TESS_VARIANT==55));
#elif PS5VK_TESS_VARIANT==25
            vkCmdDrawIndirect(coord_cb,coord_ab,sizeof(VkDrawIndirectCommand),1,0);
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_INDIRECT offset=16 count=1 vertices=3 instances=2 first_vertex=0 first_instance=3 resolver=host_submit");
#elif PS5VK_TESS_VARIANT==24
            vkCmdDraw(coord_cb,3,2,0,3);
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_INSTANCE count=2 first=3 expected_ids=3,4");
#elif PS5VK_TESS_VARIANT==22 || PS5VK_TESS_VARIANT==23
            vkCmdBindIndexBuffer(coord_cb,coord_ib,sizeof(coord_indices[0]),coord_index_type);
            vkCmdDrawIndexed(coord_cb,3,1,1,coord_base_vertex,0);
#if PS5VK_TESS_VARIANT==23
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_INDEXED type=uint32 binding_offset=4 first_index=1 vertex_offset=-65536 indices=65541,65538,65543");
#else
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_INDEXED type=uint16 binding_offset=2 first_index=1 vertex_offset=1 indices=4,1,6");
#endif
#else
            vkCmdDraw(coord_cb,PS5VK_TESS_CONTROL_VERTICES,1,0,0);
#endif
#if PS5VK_TESS_VARIANT==17 || (PS5VK_TESS_VARIANT>=19 && PS5VK_TESS_VARIANT<=21)
            vkCmdDraw(coord_cb,PS5VK_TESS_CONTROL_VERTICES,1,0,0);
            vkCmdDraw(coord_cb,PS5VK_TESS_CONTROL_VERTICES,1,0,0);
#if PS5VK_TESS_VARIANT==21
            ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_BLEND draws=3 alpha=0.25 src=CONSTANT_ALPHA dst=ZERO op=ADD");
#elif PS5VK_TESS_VARIANT==20
            ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_BLEND draws=3 alpha=0.25 src=SRC_ALPHA dst=ZERO op=ADD");
#else
            ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_BLEND draws=3 alpha=0.25 src=SRC_ALPHA dst=ONE op=ADD");
#endif
#endif
            if(coord_shared_pipeline) {
                vkCmdBindPipeline(coord_cb,VK_PIPELINE_BIND_POINT_GRAPHICS,
                    coord_shared_pipeline);
#if PS5VK_TESS_VARIANT==55
                vkCmdDrawIndexedIndirect(coord_cb,coord_ab,sizeof(VkDrawIndexedIndirectCommand),1,0);
#elif PS5VK_TESS_VARIANT==54
                vkCmdDrawIndexed(coord_cb,3,2,1,coord_base_vertex,3);
#elif PS5VK_TESS_VARIANT==25
                vkCmdDrawIndirect(coord_cb,coord_ab,sizeof(VkDrawIndirectCommand),1,0);
#elif PS5VK_TESS_VARIANT==24
                vkCmdDraw(coord_cb,3,2,0,3);
#elif PS5VK_TESS_VARIANT==22 || PS5VK_TESS_VARIANT==23
                vkCmdDrawIndexed(coord_cb,3,1,1,coord_base_vertex,0);
#else
                vkCmdDraw(coord_cb,PS5VK_TESS_CONTROL_VERTICES,1,0,0);
#endif
#if PS5VK_TESS_VARIANT==17 || (PS5VK_TESS_VARIANT>=19 && PS5VK_TESS_VARIANT<=21)
                vkCmdDraw(coord_cb,PS5VK_TESS_CONTROL_VERTICES,1,0,0);
                vkCmdDraw(coord_cb,PS5VK_TESS_CONTROL_VERTICES,1,0,0);
#endif
            }
            vkCmdEndRenderPass(coord_cb);
            CHECK(vkEndCommandBuffer(coord_cb));
#if defined(PS5VK_TESS_PREFILL) && PS5VK_TESS_PREFILL
            /* PRE-FILL THE WHOLE TESSELLATION FACTOR RING WITH A VALID LEVEL.
             *
             * The hull is proven to store correct factors, and the
             * tessellator is proven not to produce anything. Between those
             * two facts sits an assumption nobody has tested: that the
             * geometry engine READS the factors from where the hull WROTE
             * them. The hull stores at the ring base plus the per-wave
             * tcs_factor_offset the engine itself hands it, so they agree by
             * construction - unless the engine's own read base is not this
             * ring at all.
             *
             * Filling the ENTIRE extent with 2.0f removes the offset from the
             * question: wherever in this ring the engine reads, it finds a
             * legal tessellation level. If the domain then executes, the hull
             * writes to a place the engine does not read and the whole
             * remaining problem is an addressing one. If it still does not,
             * the engine is not reading this ring at all - or not running the
             * tessellator - and that is a different and much more structural
             * answer.
             *
             * The hull overwrites its own four words afterwards with the same
             * 2.0 and a 1.0 inner, which is still a legal patch, so this does
             * not fabricate a result the shader would not have produced. */
            {
                const uint32_t *pf_table=
                    (const uint32_t *)coord_native->pair->tess_rings;
                volatile uint32_t *pf=(volatile uint32_t *)(uintptr_t)
                    (((uint64_t)pf_table[21]<<32)|pf_table[20]);
                const uint32_t pf_words=pf_table[22]/4u;
                for(uint32_t i=0;i<pf_words;++i)pf[i]=0x40000000u;
                for(uintptr_t line=(uintptr_t)pf;line<(uintptr_t)pf+pf_words*4u;line+=64u)
                    __asm__ volatile("clflush (%0)" : : "r"(line) : "memory");
                __asm__ volatile("mfence" : : : "memory");
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_PREFILL variant=%s words=%u value=40000000",
                    PS5VK_TESS_CONTROL_NAME,pf_words);
            }
#endif
#if defined(PS5VK_TESS_DEFAULTS_DUMP) && PS5VK_TESS_DEFAULTS_DUMP
            /* WHAT ELSE IS IN AGC's REGISTER-DEFAULTS LIBRARY.
             *
             * sceAgcGetRegisterDefaults() returns a root with 137 keyed
             * entries indexing 84 context blocks plus shader and user-config
             * tables. This driver uses EXACTLY ONE of them - the MRT0 colour
             * target, key 0x38e92c91 - and ignores the rest.
             *
             * That is worth looking at now because of where the search has
             * ended up. Every register this driver knows to write is correct
             * and the tessellator still never runs, which is the signature of
             * state the platform expects a title to apply and this driver has
             * never heard of. A keyed block containing the tessellation
             * context registers would be exactly that, and nothing in this
             * task has ever enumerated the table.
             *
             * Read-only: every block is inspected and nothing is programmed.
             * Bounds are explicit - context offsets are below 0x400, so a
             * block is walked only while its entries look like context
             * registers and never past a fixed cap. */
            {
                const struct ps5_agc_register_defaults *root=
                    (const struct ps5_agc_register_defaults *)
                    sceAgcGetRegisterDefaults();
                if(!root || !root->type_index_pairs) {
                    ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_DEFAULTS root=absent");
                } else {
                    ps5log_printf(PS5LOG_MARK,
                        "PS5VK_TESS_DEFAULTS count=%u cx=%d sh=%d uc=%d t3=%d",
                        root->count,root->table_cx?1:0,root->table_sh?1:0,
                        root->table_uc?1:0,root->table_3?1:0);
                    for(uint32_t i=0;i<root->count && i<160u;++i) {
                        const struct ps5_agc_type_index *e=
                            &root->type_index_pairs[i];
                        const uint32_t bank=ps5_agc_type_index_bank(e);
                        const uint32_t idx=ps5_agc_type_index_value(e);
                        uint32_t first=0xffffu,n=0,tess=0;
                        if(bank==0u && root->table_cx && idx<84u &&
                           root->table_cx[idx]) {
                            const ps5_agc_register *b=root->table_cx[idx];
                            for(;n<64u;++n) {
                                const uint32_t off=b[n].offset;
                                if(off>=0x400u)break;
                                if(n==0)first=off;
                                if(off==0x2d4u||off==0x2d5u||off==0x2d6u||
                                   off==0x2dbu||off==0x286u||off==0x287u)
                                    tess=off;
                            }
                        }
                        ps5log_printf(PS5LOG_MARK,
                            "PS5VK_TESS_DEFAULTS i=%u key=%08x bank=%u idx=%u "
                            "first=%03x n=%u tess=%03x",
                            i,e->key,bank,idx,first,n,tess);
                    }
                }
            }
#endif
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 3
            struct ps5vk_tess_ring_lease tf_lease={0};
#endif
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY
            {
                uint64_t ring_address=UINT64_MAX;
                uint32_t ring_size=UINT32_MAX;
                uint16_t first=UINT16_MAX,second=UINT16_MAX;
                int32_t ring_rc=sceAgcDriverGetTFRing(&ring_address,&ring_size);
                int32_t offchip_rc=sceAgcDriverGetHsOffchipParam(&first,&second);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_DRIVER_QUERY ring_rc=%d base=%08x%08x size_raw=%08x offchip_rc=%d first_raw=%04x second_raw=%04x",
                    ring_rc,(uint32_t)(ring_address>>32),(uint32_t)ring_address,
                    ring_size,offchip_rc,(unsigned)first,(unsigned)second);
#if PS5VK_TESS_RING_QUERY == 3
                if(ring_rc || ring_address==UINT64_MAX || ring_size==UINT32_MAX)
                    fail("tess ring snapshot",VK_ERROR_UNKNOWN);
                const uint32_t *table=(const uint32_t *)coord_native->pair->tess_rings;
                const uint64_t own=((uint64_t)table[21]<<32)|table[20];
                if((own&255u) || table[22]<65536u*4u)
                    fail("tess bound ring bounds",VK_ERROR_UNKNOWN);
                const struct ps5vk_tess_ring_ops ops={NULL,tess_ring_get,tess_ring_set};
                int bind_rc=ps5vk_tess_ring_bind(&tf_lease,&ops,own,65536u);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_RING_BOUND rc=%d state=%u match=%u raw_size=%08x",
                    bind_rc,(unsigned)tf_lease.state,(unsigned)(bind_rc==0),65536u);
                if(bind_rc) {
                    fail("tess ring bind",VK_ERROR_DEVICE_LOST);
                }
#elif PS5VK_TESS_RING_QUERY == 2
                /* No GPU work between set/get/restore. Small raw size fits
                 * this owned allocation whether the API counts bytes or
                 * dwords. This experiment does NOT establish size units. */
                if(ring_rc!=0 || ring_address==UINT64_MAX || ring_size==UINT32_MAX)
                    fail("tess ring snapshot",VK_ERROR_UNKNOWN);
                const uint32_t *table=(const uint32_t *)coord_native->pair->tess_rings;
                const uint64_t own=((uint64_t)table[21]<<32)|table[20];
                if((own&255u) || table[22]<4096u*4u)
                    fail("tess ring roundtrip bounds",VK_ERROR_UNKNOWN);
                uint64_t got=UINT64_MAX,restored=UINT64_MAX;
                uint32_t got_size=UINT32_MAX,restored_size=UINT32_MAX;
                int32_t set_rc=sceAgcDriverSetTFRing(own,4096u);
                int32_t get_rc=sceAgcDriverGetTFRing(&got,&got_size);
                /* Restore even when the attempted setter/getter failed. */
                int32_t restore_rc=sceAgcDriverSetTFRing(ring_address,ring_size);
                int32_t verify_rc=sceAgcDriverGetTFRing(&restored,&restored_size);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_RING_ROUNDTRIP set=%d get=%d match=%u size=%08x restore=%d verify=%d restored=%u",
                    set_rc,get_rc,(unsigned)(got==own && got_size==4096u),got_size,
                    restore_rc,verify_rc,(unsigned)(restored==ring_address && restored_size==ring_size));
                if(restore_rc || verify_rc || restored!=ring_address || restored_size!=ring_size)
                    fail("tess ring restore",VK_ERROR_UNKNOWN);
#endif
            }
#endif
            tess_receipt(PS5VK_TESS_CONTROL_NAME,coord_native,PS5VK_TESS_CONTROL_VERTICES);
            /* Bound diagnostic waits so a stalled candidate cannot silently
             * wait forever. Earlier candidates stalled; ring-bound controls
             * now retire and pass their pixel oracle. Neither behavior is a
             * guarantee for a new candidate. Unretired memory observations
             * are diagnostics only: zero bytes do not prove non-execution,
             * and sentinel/pre-filled bytes do not prove GPU writes. The
             * bound-ring path below retains backing on unresolved work and
             * restores prior device state only after successful completion. */
            VkFence coord_fence=VK_NULL_HANDLE;
            VkFenceCreateInfo coord_fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            CHECK(vkCreateFence(d,&coord_fi,NULL,&coord_fence));
            VkSubmitInfo coord_submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount=1,.pCommandBuffers=&coord_cb};
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 3
            if(ps5vk_tess_ring_submitting(&tf_lease))
                fail("tess ring submitting",VK_ERROR_DEVICE_LOST);
#endif
            CHECK(vkQueueSubmit(queue,1,&coord_submit,coord_fence));
            const VkResult coord_wait=
                vkWaitForFences(d,1,&coord_fence,VK_TRUE,300000000ull);
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY >= 3
            /* An unresolved submission retains its backing until process
             * closure; never destroy the pipeline or restore a live ring. */
            if(coord_wait!=VK_SUCCESS)
                fail("tess bound ring fence",VK_ERROR_DEVICE_LOST);
#endif
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 3
            {
                if(ps5vk_tess_ring_completed(&tf_lease))
                    fail("tess ring completed",VK_ERROR_DEVICE_LOST);
                int rc=ps5vk_tess_ring_restore(&tf_lease);
                ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_RING_RESTORED rc=%d state=%u match=%u",
                    rc,(unsigned)tf_lease.state,(unsigned)(rc==0));
                if(rc)
                    fail("tess bound ring restore",VK_ERROR_DEVICE_LOST);
            }
#endif
            if(coord_wait!=VK_SUCCESS) {
                const uint32_t *coord_table=
                    (const uint32_t *)coord_native->pair->tess_rings;
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_GPU_REGS size=%08x offchip=%08x base=%08x hi=%08x stages=%08x config=%08x tf=%08x ge=%08x",
                    coord_table[32],coord_table[33],coord_table[34],coord_table[35],
                    coord_table[36],coord_table[37],coord_table[38],coord_table[39]);
                const uint64_t factor_va=
                    ((uint64_t)coord_table[21]<<32)|coord_table[20];
                const volatile uint32_t *factors=
                    (const volatile uint32_t *)(uintptr_t)factor_va;
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_STALL variant=%s wait=%d factor_va=%08x%08x "
                    "f0=%08x f1=%08x f2=%08x f3=%08x f4=%08x f5=%08x "
                    "f6=%08x f7=%08x",
                    PS5VK_TESS_CONTROL_NAME,(int)coord_wait,
                    coord_table[21],coord_table[20],
                    factors[0],factors[1],factors[2],factors[3],
                    factors[4],factors[5],factors[6],factors[7]);
                vkDestroyFence(d,coord_fence,NULL);
                vkDestroyPipeline(d,coord_pipeline,NULL);
                vkDestroyCommandPool(d,coord_pool,NULL);
                goto coord_done;
            }
            vkDestroyFence(d,coord_fence,NULL);
            /* The tessellation factors the merged LS/HS program stored, read
             * back after the draw RETIRED rather than out of a stalled
             * process. This is the fork the whole remaining search turns on
             * and it deserves to be measured rather than assumed in either
             * direction: the control half writes outer 2.0, 2.0, 2.0 and
             * inner 1.0 into a ring memset to zero at create, so 0x40000000
             * three times and 0x3f800000 say the hull executed and reached
             * its store, and the failure that remains is downstream in the
             * tessellator, the domain or rasterisation. All zeros say the
             * hull still does not store, with the draw now retiring anyway.
             * The address comes from the ring table's own entry 5 rather
             * than a constant, so the record carries the address it read.
             *
             * THE CAVEAT, stated before the answer rather than after it, and
             * unchanged from the stall-path read: the ring backs the
             * PIPELINE's own allocation, not the readback memory this
             * harness owns a handle to, so there is no invalidate to issue
             * for it here and this is a volatile load from non-coherent
             * memory. A NON-ZERO result is therefore strong - nothing
             * invents those bit patterns in a zeroed buffer - while a zero
             * result is weaker, because a stale line could in principle hide
             * a write that did happen. */
            {
                const uint32_t *coord_table=
                    (const uint32_t *)coord_native->pair->tess_rings;
                const volatile uint32_t *factors=
                    (const volatile uint32_t *)(uintptr_t)
                    (((uint64_t)coord_table[21]<<32)|coord_table[20]);
                /* SCAN THE WHOLE RING, not its first words.
                 *
                 * The earlier read printed factors[0..7] and called an
                 * all-zero result evidence about the hull. That was a flawed
                 * instrument and the flaw favoured the conclusion I was
                 * already leaning towards. The hull stores at the ring base
                 * plus tcs_factor_offset, a PER-WAVE offset the geometry
                 * engine hands the program in a system SGPR; nothing makes
                 * it zero, so the first eight words are a location the hull
                 * may simply never write. A scan of the ring's whole byte
                 * extent - taken from the same SRD the shader dereferences,
                 * entry 5 word 2 - cannot miss a store the way a fixed
                 * window can, and it reports WHERE the first ones landed so
                 * the offset itself becomes a measurement. */
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_GPU_REGS size=%08x offchip=%08x base=%08x hi=%08x stages=%08x config=%08x tf=%08x ge=%08x",
                    coord_table[32],coord_table[33],coord_table[34],coord_table[35],
                    coord_table[36],coord_table[37],coord_table[38],coord_table[39]);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_GPU_LAUNCH max=%08x min=%08x prim=%08x pc=%08x es=%08x gs1=%08x gs2=%08x ls=%08x",
                    coord_table[40],coord_table[41],coord_table[42],coord_table[43],
                    coord_table[44],coord_table[45],coord_table[46],coord_table[47]);
#if defined(PS5VK_TESS_ENTRY_WITNESS) && PS5VK_TESS_ENTRY_WITNESS
                /* Read the witness only after the successful GPU fence.
                 * Invalidate its cache line; never follow these pointers. */
                __asm__ volatile("clflush (%0)"::"r"((const char *)coord_table+192):"memory");
                __asm__ volatile("mfence":::"memory");
                const volatile uint32_t *entry=coord_table+48;
                ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_ENTRY_DEST table=%08x%08x",
                    (uint32_t)((uintptr_t)coord_table>>32),(uint32_t)(uintptr_t)coord_table);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_ENTRY raw=1 s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x s6=%08x s7=%08x",
                    entry[0],entry[1],entry[2],entry[3],entry[4],entry[5],entry[6],entry[7]);
#if PS5VK_TESS_VARIANT==5
                /* Owned ring SRD, checked against allocation-relative layout;
                 * inspect only after fence and invalidate before CPU reads. */
                const uint64_t offchip_va=((uint64_t)coord_table[25]<<32)|coord_table[24];
                const uint32_t offchip_bytes=coord_table[26];
                const uint64_t owned_factor_va=((uint64_t)coord_table[21]<<32)|coord_table[20];
                if(offchip_va!=(uintptr_t)coord_table+256u ||
                   owned_factor_va<offchip_va || !offchip_bytes ||
                   offchip_bytes>owned_factor_va-offchip_va)
                    fail("offchip witness bounds",VK_ERROR_UNKNOWN);
                const volatile uint32_t *offchip=(const volatile uint32_t *)(uintptr_t)offchip_va;
                for(uint32_t i=0;i<offchip_bytes;i+=64)
                    __asm__ volatile("clflush (%0)"::"r"((const char *)offchip+i):"memory");
                __asm__ volatile("mfence":::"memory");
                uint32_t count=0;
                for(uint32_t i=0;i<offchip_bytes/4u;++i) {
                    const uint32_t value=offchip[i];
                    if(!value)continue;
                    if(count<24u)ps5log_printf(PS5LOG_MARK,
                        "PS5VK_TESS_OFFCHIP_WORD index=%u value=%08x",i,value);
                    ++count;
                }
                ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_OFFCHIP bytes=%u nonzero=%u",offchip_bytes,count);
#endif
#endif
                const uint32_t extent=coord_table[22];
                const uint32_t words=extent/4u;
                /* Every non-zero word, not the first four. Two runs of the
                 * identical payload reported four and then five, which means
                 * the truncation was hiding part of the answer: a factor
                 * store whose word count varies between runs is either more
                 * than one wave writing or a layout this task has not
                 * understood, and neither can be read off a truncated list. */
                uint32_t nonzero=0,at[8]={0},val[8]={0};
                for(uint32_t i=0;i<words;++i) {
                    const uint32_t v=factors[i];
                    if(!v)continue;
                    if(nonzero<8){at[nonzero]=i;val[nonzero]=v;}
                    ++nonzero;
                }
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_FACTORS variant=%s factor_va=%08x%08x "
                    "extent=%08x nonzero=%u w0=%u:%08x w1=%u:%08x "
                    "w2=%u:%08x w3=%u:%08x w4=%u:%08x w5=%u:%08x "
                    "w6=%u:%08x w7=%u:%08x",
                    PS5VK_TESS_CONTROL_NAME,coord_table[21],coord_table[20],
                    extent,nonzero,at[0],val[0],at[1],val[1],at[2],val[2],
                    at[3],val[3],at[4],val[4],at[5],val[5],at[6],val[6],
                    at[7],val[7]);
            }
            CHECK(vkInvalidateMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
                .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,
                .offset=0,.size=VK_WHOLE_SIZE}));
#if PS5VK_TESS_VARIANT==4
            /* Did the evaluation half execute? This is the whole point of the
             * variant, and unlike every factor-ring read in this task it
             * needs no caveat: the buffer is an ordinary Vulkan allocation the
             * harness owns, invalidated through the API before it is read. */
            CHECK(vkInvalidateMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
                .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory=coord_witness_memory,.offset=0,.size=VK_WHOLE_SIZE}));
            {
                const uint32_t *w=(const uint32_t *)coord_witness_mapped;
                const float *c=(const float *)(w+4);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_DOMAIN_EXEC variant=%s invocations=%u vertex=%u "
                    "c0=%08x %08x %08x c1=%08x %08x %08x "
                    "c2=%08x %08x %08x c3=%08x %08x %08x",
                    PS5VK_TESS_CONTROL_NAME,w[0],w[1],
                    ((const uint32_t *)c)[0],((const uint32_t *)c)[1],
                    ((const uint32_t *)c)[2],((const uint32_t *)c)[3],
                    ((const uint32_t *)c)[4],((const uint32_t *)c)[5],
                    ((const uint32_t *)c)[6],((const uint32_t *)c)[7],
                    ((const uint32_t *)c)[8],((const uint32_t *)c)[9],
                    ((const uint32_t *)c)[10],((const uint32_t *)c)[11]);
            }
#endif
            static uint8_t coord_detiled[PS5VK_GEOMETRY_EXTENT*PS5VK_GEOMETRY_EXTENT*4];
            if(ps5vk_rgba8_64k_rx_detile(coord_detiled,sizeof(coord_detiled),map,
                (size_t)stride,extent,extent))fail("tess-coord-detile",-1);
            unsigned long long c_ink=0,c_expected=0,c_covered=0,c_missing=0,
                c_foreign=0,c_wrong=0;
            unsigned first_wrong_x=0,first_wrong_y=0;
            uint8_t first_wrong[4]={0,0,0,0},first_want[3]={0,0,0};
#if PS5VK_TESS_VARIANT==18
            unsigned alpha_wrong=0,alpha_min=255,alpha_max=0;
#endif
            /* The oracle, derived from the evaluation half's own math BEFORE
             * the run and independent of the image.
             *
             * The evaluation half maps the domain point to
             *   ndc = (u*1.8-0.9, v*1.8-0.9)
             * so the covered region is the triangle (-0.9,-0.9),(0.9,-0.9),
             * (-0.9,0.9) - note this is NOT the vertex half's triangle, whose
             * third corner is (0,0.9), so coverage alone already separates
             * "the tessellator ran" from "the vertex half was rasterised".
             *
             * Vulkan framebuffer coordinates: origin upper left, pixel centre
             * at +0.5, viewport (0,0,extent,extent), so
             *   ndc = 2*(pixel+0.5)/extent - 1
             * which is the inverse of the mapping the geometry witness's own
             * oracle uses (src/geometry_witness.c: pixel=(ndc+1)*0.5*extent).
             *
             * Colour is linear TessCoord in the control TES, so interpolation
             * is (u,v,0.5), independent of generated interior vertices and
             * triangulation. Do not quantize the shader output: inner level1
             * with outer levels>1 is treated as 1+epsilon by Vulkan and is
             * subdivided; generated coordinates need not be multiples of0.5.
             * A quantized shader requires a different, piecewise oracle. */
#if PS5VK_TESS_VARIANT==6 || PS5VK_TESS_VARIANT==7 || PS5VK_TESS_VARIANT==24 || PS5VK_TESS_VARIANT==25 || PS5VK_TESS_VARIANT==54 || PS5VK_TESS_VARIANT==55
            /* Two disjoint triangles; colour blue identifies which patch
             * supplied the interpolated attributes. Invert each rectangle's
             * affine map independently of GPU-produced coordinates. */
            const float eps=1.5f*(2.0f/(float)extent)/0.8f;
#else
            const float eps=1.5f*(2.0f/(float)extent)/1.8f;
#endif
            for(unsigned y=0;y<extent;++y)for(unsigned x=0;x<extent;++x) {
                const float ndc_x=2.0f*((float)x+0.5f)/(float)extent-1.0f;
                const float ndc_y=2.0f*((float)y+0.5f)/(float)extent-1.0f;
#if PS5VK_TESS_VARIANT>=44 && PS5VK_TESS_VARIANT<=52
                if(extent!=64u)fail("discard oracle extent",-1);
                const int discard_patch=ps5vk_tess_discard_pixel((PS5VK_TESS_VARIANT-44)/3,x,y);
                const float u=(float)(discard_patch+1)/32.0f,v=1.0f,blue=0.5f;
                (void)eps;(void)ndc_x;(void)ndc_y;
#elif PS5VK_TESS_VARIANT>=35 && PS5VK_TESS_VARIANT<=43
                if(extent!=64u)fail("point matrix oracle extent",-1);
                const float u=((float)x-4.0f)/54.0f,v=((float)y-4.0f)/54.0f,blue=0.5f;
                (void)eps;(void)ndc_x;(void)ndc_y;
#elif PS5VK_TESS_VARIANT==29
                if(extent!=64u)fail("level64 oracle extent",-1);
                const float u=1.0f,v=0.0f,blue=0.0f;
                (void)eps;(void)ndc_x;(void)ndc_y;
#elif PS5VK_TESS_VARIANT==16
                if(extent!=64u)fail("TES-GS oracle extent",-1);
                const float u=(ndc_x+0.609375f)/1.5f;
                const float v=(ndc_y+0.859375f)/1.5f,blue=0.5f;
                (void)eps;
#elif PS5VK_TESS_VARIANT==13 || PS5VK_TESS_VARIANT==14 || PS5VK_TESS_VARIANT==15 || PS5VK_TESS_VARIANT==17
                if(extent!=64u)fail("point oracle extent",-1);
                const float u=(ndc_x+0.734375f)/1.5f;
                const float v=(ndc_y+0.734375f)/1.5f,blue=0.5f;
                (void)eps;
#elif PS5VK_TESS_VARIANT==9
                if(extent!=64u)fail("isoline oracle extent",-1);
                const float u=(ndc_x+0.734375f)/1.5f;
                const float v=(ndc_y+0.484375f)/2.0f,blue=0.5f;
                (void)eps;
#elif PS5VK_TESS_VARIANT==6 || PS5VK_TESS_VARIANT==7 || PS5VK_TESS_VARIANT==24 || PS5VK_TESS_VARIANT==25 || PS5VK_TESS_VARIANT==54 || PS5VK_TESS_VARIANT==55
                const unsigned patch=ndc_x>=0.0f;
                const float u=(ndc_x+0.9f-(float)patch)/0.8f;
                const float v=(ndc_y+0.9f)/1.8f;
                const float blue=patch?0.75f:0.25f;
#else
                const float u=(ndc_x+0.9f)/1.8f,v=(ndc_y+0.9f)/1.8f;
                const float blue=0.5f;
#endif
                const uint8_t *pxb=coord_detiled+4*((size_t)y*extent+x);
                const int is_ink=pxb[0]||pxb[1]||pxb[2];
                if(is_ink)++c_ink;
#if PS5VK_TESS_VARIANT>=44 && PS5VK_TESS_VARIANT<=52
                const int inside=discard_patch>=0;
                const int outside=!inside;
#elif PS5VK_TESS_VARIANT>=35 && PS5VK_TESS_VARIANT<=43
                const int inside=ps5vk_tess_point_inside((PS5VK_TESS_VARIANT-35)/3,
                    (PS5VK_TESS_VARIANT-35)%3,x,y);
                const int outside=!inside;
#elif PS5VK_TESS_VARIANT==29
                /* Fixed 65-point atlas, independent of shader rounding.
                 * Level32 misses 32 required pixels; judge every pixel. */
                const int inside=(x>=7u&&x<=49u&&(x-7u)%6u==0u&&
                                  y>=7u&&y<=49u&&(y-7u)%6u==0u)||
                                 (x==7u&&y==55u);
                const int outside=!inside;
#elif PS5VK_TESS_VARIANT==16
                const int inside=(x==12u||x==36u||x==60u)&&
                                 (y==4u||y==28u||y==52u);
                const int outside=!inside;
#elif PS5VK_TESS_VARIANT==13 || PS5VK_TESS_VARIANT==14 || PS5VK_TESS_VARIANT==15 || PS5VK_TESS_VARIANT==17
                /* All pixels are judged; no edge-tolerance band for points. */
                const int inside=(x==8u||x==32u||x==56u)&&
                                 (y==8u||y==32u||y==56u);
                const int outside=!inside;
#elif PS5VK_TESS_VARIANT==9
                const int row=y==16u||y==48u;
                const int inside=row&&x>=10u&&x<=54u;
                const int outside=!(row&&x>=7u&&x<=58u);
#elif PS5VK_TESS_VARIANT==8 || (PS5VK_TESS_VARIANT>=18 && PS5VK_TESS_VARIANT<=21)
                const int inside=u>eps&&v>eps&&u<1.0f-eps&&v<1.0f-eps;
                const int outside=u<-eps||v<-eps||u>1.0f+eps||v>1.0f+eps;
#else
                const int inside=u>eps&&v>eps&&(u+v)<1.0f-eps;
                const int outside=u<-eps||v<-eps||(u+v)>1.0f+eps;
#endif
                if(outside) {
                    if(is_ink)++c_foreign;
                    continue;
                }
                /* The one-pixel band along the edges is not under test here:
                 * which side of a shared edge owns a sample is a rasterisation
                 * rule, not a tessellation result. */
                if(!inside)continue;
                ++c_expected;
                if(!is_ink){++c_missing;continue;}
                ++c_covered;
#if PS5VK_TESS_VARIANT==18
                /* Independent alpha check: the RGB oracle alone cannot prove
                 * delivery of the fourth component used by blending. */
                const unsigned alpha=pxb[3];
                if(alpha<alpha_min)alpha_min=alpha;
                if(alpha>alpha_max)alpha_max=alpha;
                if(alpha<61u||alpha>67u)++alpha_wrong;
#endif
#if PS5VK_TESS_VARIANT==14 || PS5VK_TESS_VARIANT==15
                const float expect[3]={x<extent/2?0.25f:0.75f,
                    x<extent/2?0.5f:0.25f,x<extent/2?0.75f:0.5f};
                (void)u;(void)v;(void)blue;
#else
#if PS5VK_TESS_VARIANT==33
                /* Interpolated u indexes four different distances.
                 * Oracle selects their analytic u/v/half/one values directly. */
                const float distances[4]={u,v,0.5f,1.0f};
                const float expect[3]={distances[((unsigned)(u*4.0f))&3u],0.0f,0.0f};
                (void)blue;
#elif PS5VK_TESS_VARIANT==16
                const float expect[3]={v,blue,u};
#elif PS5VK_TESS_VARIANT==20 || PS5VK_TESS_VARIANT==21
                /* Destination ZERO: each draw replaces with source*alpha. */
                const float expect[3]={0.25f*u,0.25f*v,0.25f*blue};
#elif PS5VK_TESS_VARIANT==17 || PS5VK_TESS_VARIANT==19
                /* Three SRC_ALPHA/ONE blends, source alpha 1/4, black clear.
                 * Disabled blending would return the full source, not 3/4. */
                const float expect[3]={0.75f*u,0.75f*v,0.75f*blue};
#else
                const float expect[3]={u,v,blue};
#endif
#endif
                int wrong=0;
                for(int ch=0;ch<3;++ch) {
                    const unsigned got=pxb[ch];
                    const unsigned want=(unsigned)(expect[ch]*255.0f+0.5f);
                    const unsigned diff=got>want?got-want:want-got;
                    if(diff>3u)wrong=1;
                }
                if(wrong) {
                    if(!c_wrong) {
                        first_wrong_x=x;first_wrong_y=y;
                        for(int ch=0;ch<4;++ch)first_wrong[ch]=pxb[ch];
                        for(int ch=0;ch<3;++ch)
                            first_want[ch]=(uint8_t)(expect[ch]*255.0f+0.5f);
                    }
                    ++c_wrong;
                }
            }
#if PS5VK_TESS_VARIANT==1 || PS5VK_TESS_VARIANT>=5
            /* A: the patch must appear, in the right place, with the right
             * field. */
            const int coord_verified=c_expected>0&&c_missing==0&&c_foreign==0&&
                c_wrong==0
#if PS5VK_TESS_VARIANT==18
                &&alpha_wrong==0
#endif
                ;
#else
            /* B: zero outer levels discard the patch, so NOTHING may be
             * rasterised anywhere in the target. */
            const int coord_verified=c_ink==0;
#endif
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_CONTROL variant=%s rc=%d created=1 vertices=%u "
                "ink=%llu expected=%llu covered=%llu missing=%llu foreign=%llu "
                "wrong_color=%llu digest=%016llx verified=%d "
                "first_wrong=%02x%02x%02x%02x want=%02x%02x%02x at=%u,%u",
                PS5VK_TESS_CONTROL_NAME,(int)coord_rc,PS5VK_TESS_CONTROL_VERTICES,
                (unsigned long long)c_ink,(unsigned long long)c_expected,
                (unsigned long long)c_covered,(unsigned long long)c_missing,
                (unsigned long long)c_foreign,(unsigned long long)c_wrong,
                (unsigned long long)geometry_digest(coord_detiled,
                    sizeof(coord_detiled)),
                coord_verified,
                first_wrong[0],first_wrong[1],first_wrong[2],first_wrong[3],
                first_want[0],first_want[1],first_want[2],
                first_wrong_x,first_wrong_y);
#if PS5VK_TESS_VARIANT==18
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_ALPHA expected=64 tolerance=3 samples=%llu wrong=%u min=%u max=%u",
                (unsigned long long)c_covered,alpha_wrong,alpha_min,alpha_max);
#endif
            vkDestroyPipeline(d,coord_pipeline,NULL);
            if(coord_shared_pipeline)vkDestroyPipeline(d,coord_shared_pipeline,NULL);
            vkDestroyCommandPool(d,coord_pool,NULL);
#if PS5VK_TESS_VARIANT==25 || PS5VK_TESS_VARIANT==55
            vkDestroyBuffer(d,coord_ab,NULL);
            vkFreeMemory(d,coord_am,NULL);
#endif
#if PS5VK_TESS_VARIANT==22 || PS5VK_TESS_VARIANT==23 || PS5VK_TESS_VARIANT==54 || PS5VK_TESS_VARIANT==55
            vkDestroyBuffer(d,coord_ib,NULL);
            vkFreeMemory(d,coord_im,NULL);
#endif
        } else {
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_CONTROL variant=%s rc=%d created=0 site=%u",
                PS5VK_TESS_CONTROL_NAME,(int)coord_rc,
                ps5vk_pipeline_refusal_site());
        }
#if PS5VK_TESS_VARIANT==56
        vkDestroyPipelineLayout(d,coord_layout,NULL);
#endif
    }
coord_done:
    for(unsigned i=0;i<coord_stage_count;++i)vkDestroyShaderModule(d,coord_modules[i],NULL);
#undef PS5VK_TESS_CONTROL_NAME
#undef PS5VK_TESS_CONTROL_CODE
#endif
#if PS5VK_TESS_PROBE && PS5VK_TESS_VARIANT==3
    extern unsigned ps5vk_pipeline_refusal_site(void);
    /* WITNESS C (PS5VK_TESS_VARIANT==3), report-only. Two triangle patches: the left
     * tessellated at level three, the right at level one. The evaluation half
     * paints the QUANTISED tessCoord field - colour = (floor(u*n)/n,
     * floor(v*n)/n, patch) - so the image names the sub-triangle each pixel
     * came from and the tessellator's levels are observable: a run without a
     * working tessellator paints a different, computable image. The oracle
     * derives every covered pixel's expected colour from the same math: the
     * containing sub-triangle of the pixel's own tessCoord, its three corners'
     * quantised colours, and the interpolation between them. */
    VkShaderModule tess_modules[4];
    const struct { const uint32_t *code; size_t bytes; } tess_codes[4]={
        {ps5vk_runtime_tess_witness_vertex,sizeof(ps5vk_runtime_tess_witness_vertex)},
        {ps5vk_runtime_tess_witness_control,sizeof(ps5vk_runtime_tess_witness_control)},
        {ps5vk_runtime_tess_witness_evaluation,sizeof(ps5vk_runtime_tess_witness_evaluation)},
        {ps5vk_runtime_tess_witness_fragment,sizeof(ps5vk_runtime_tess_witness_fragment)}};
    for(unsigned i=0;i<4;++i) {
        VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=tess_codes[i].bytes,.pCode=tess_codes[i].code};
        CHECK(vkCreateShaderModule(d,&mi,NULL,&tess_modules[i]));
    }
    {
        const VkShaderStageFlagBits tess_stages[4]={
            VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,VK_SHADER_STAGE_FRAGMENT_BIT};
        VkPipelineShaderStageCreateInfo tess_stage_infos[4];
        for(unsigned i=0;i<4;++i)
            tess_stage_infos[i]=(VkPipelineShaderStageCreateInfo){
                .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage=tess_stages[i],.module=tess_modules[i],.pName="main"};
        VkPipelineInputAssemblyStateCreateInfo tess_ia={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
        VkPipelineTessellationStateCreateInfo tess_state_info={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
            .patchControlPoints=3};
        VkPipelineVertexInputStateCreateInfo tess_vi={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo tess_raster={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
        VkPipelineMultisampleStateCreateInfo tess_ms={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkViewport tess_vp_v={0,0,(float)extent,(float)extent,0,1};
        VkRect2D tess_vp_s={{0,0},{extent,extent}};
        VkPipelineViewportStateCreateInfo tess_vp={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&tess_vp_v,.scissorCount=1,.pScissors=&tess_vp_s};
        VkPipelineColorBlendAttachmentState tess_blend_a={.colorWriteMask=15};
        VkPipelineColorBlendStateCreateInfo tess_blend={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount=1,.pAttachments=&tess_blend_a};
        VkGraphicsPipelineCreateInfo tess_pi={
            .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .layout=layout,.renderPass=pass,.stageCount=4,
            .pStages=tess_stage_infos,.pVertexInputState=&tess_vi,
            .pInputAssemblyState=&tess_ia,.pTessellationState=&tess_state_info,
            .pRasterizationState=&tess_raster,.pMultisampleState=&tess_ms,
            .pViewportState=&tess_vp,.pColorBlendState=&tess_blend};
        VkPipeline tess_pipeline;
        const VkResult tess_rc=vkCreateGraphicsPipelines(d,0,1,&tess_pi,NULL,&tess_pipeline);
        unsigned long long t_expected=0,t_covered=0,t_missing=0,t_foreign=0,t_wrong=0;
        uint32_t t_stages_en=0,t_ls_hs=0,t_tf=0;
        if(tess_rc==VK_SUCCESS && tess_pipeline) {
            /* The pre-raster program a tessellation pipeline packages is the
             * domain half; the pair must carry the hull state the create path
             * prepared, and this log carries it back as the launch record. */
            const struct ps5vk_native_graphics_pipeline *tess_native=tess_pipeline->graphics_state;
            if(!tess_native || !tess_native->pair || !tess_native->pair->ready ||
               !tess_native->pair->tessellation)
                fail("tess-pipeline",-1);
            /* The stage enables live in pair->tess_state[0], which the create
             * path programs directly; they are NOT republished in the domain
             * program's own linked context block. The previous form only
             * assigned t_stages_en if a scan of pair->runtime_vertex.context[]
             * found offset 0x2d5 - and runtime_vertex holds the DOMAIN program
             * for a tessellation pipeline, whose linked block carries no 0x2d5
             * at all. The scan never matched, so the witness reported
             * stages_en=00000000 for a pipeline whose enables were programmed.
             * Read the register the driver actually wrote. */
            t_stages_en=tess_native->pair->tess_state[0].value;
            t_ls_hs=tess_native->pair->tess_state[1].value;
            for(unsigned i=0;i<tess_native->pair->runtime_hull.header.num_cx_registers;++i)
                if(tess_native->pair->runtime_hull.context[i].offset==0x2db)
                    t_tf=tess_native->pair->runtime_hull.context[i].value;
            VkCommandPool tess_pool;
            VkCommandPoolCreateInfo tess_pci={
                .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex=0};
            CHECK(vkCreateCommandPool(d,&tess_pci,NULL,&tess_pool));
            VkCommandBuffer tess_cb=VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo tess_cbi={
                .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool=tess_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount=1};
            CHECK(vkAllocateCommandBuffers(d,&tess_cbi,&tess_cb));
            VkCommandBufferBeginInfo tess_begin={
                .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(vkBeginCommandBuffer(tess_cb,&tess_begin));
            if (PS5VK_TESS_NO_DRAW) {
                /* The create-only diagnostic: the pipeline exists and the
                 * launch state is programmed; the draw is the bisect step. */
                tess_receipt("C-offchip-witness",tess_native,0u);
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_PROBE rc=%d created=1 draw_skipped=1 "
                    "stages_en=%08x ls_hs_config=%08x tf_param=%08x primitive=%08x",
                    (int)tess_rc,t_stages_en,t_ls_hs,t_tf,
                    tess_native->pair->uc.vgt_primitive_type.value);
                vkDestroyPipeline(d,tess_pipeline,NULL);
                vkDestroyCommandPool(d,tess_pool,NULL);
                goto tess_done;
            }
            VkClearValue tess_clear={.color={.float32={0.0f,0.0f,0.0f,1.0f}}};
            VkRenderPassBeginInfo tess_rbi={
                .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass=pass,.framebuffer=fb,
                .renderArea={{0,0},{extent,extent}},
                .clearValueCount=1,.pClearValues=&tess_clear};
            vkCmdBeginRenderPass(tess_cb,&tess_rbi,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(tess_cb,VK_PIPELINE_BIND_POINT_GRAPHICS,tess_pipeline);
            /* Two triangle patches, three vertices each. */
            vkCmdDraw(tess_cb,6,1,0,0);
            vkCmdEndRenderPass(tess_cb);
            CHECK(vkEndCommandBuffer(tess_cb));
            tess_receipt("C-offchip-witness",tess_native,6u);
            VkSubmitInfo tess_submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount=1,.pCommandBuffers=&tess_cb};
            CHECK(vkQueueSubmit(queue,1,&tess_submit,VK_NULL_HANDLE));
            CHECK(vkQueueWaitIdle(queue));
            CHECK(vkInvalidateMappedMemoryRanges(d,1,&(VkMappedMemoryRange){
                .sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,
                .offset=0,.size=VK_WHOLE_SIZE}));
            static uint8_t tess_detiled[PS5VK_GEOMETRY_EXTENT*PS5VK_GEOMETRY_EXTENT*4];
            if(ps5vk_rgba8_64k_rx_detile(tess_detiled,sizeof(tess_detiled),map,
                (size_t)stride,extent,extent))fail("tess-detile",-1);
            /* The two patches' screen-space corners, derived from the witness
             * vertex stage's own NDC list through the viewport mapping. */
            const float corners[2][3][2]={
                {{-0.9f,-0.9f},{-0.1f,-0.9f},{-0.5f,0.9f}},
                {{ 0.1f,-0.9f},{ 0.9f,-0.9f},{ 0.5f,0.9f}}};
            for(unsigned y=0;y<extent;++y)for(unsigned x=0;x<extent;++x) {
                const float px=(float)x+0.5f,py=(float)y+0.5f;
                const float ndc_x=px/(float)extent*2.0f-1.0f;
                const float ndc_y=1.0f-py/(float)extent*2.0f;
                int patch=-1;
                float tess[3]={0,0,0};
                for(unsigned p=0;p<2;++p) {
                    const float (*t)[2]=corners[p];
                    const float denom=(t[1][1]-t[2][1])*(t[0][0]-t[2][0])+
                        (t[2][0]-t[1][0])*(t[0][1]-t[2][1]);
                    if(denom==0.0f)continue;
                    const float a=((t[1][1]-t[2][1])*(ndc_x-t[2][0])+
                        (t[2][0]-t[1][0])*(ndc_y-t[2][1]))/denom;
                    const float b=((t[2][1]-t[0][1])*(ndc_x-t[2][0])+
                        (t[0][0]-t[2][0])*(ndc_y-t[2][1]))/denom;
                    const float c=1.0f-a-b;
                    if(a>=-0.002f&&b>=-0.002f&&c>=-0.002f){
                        patch=(int)p;tess[0]=a;tess[1]=b;tess[2]=c;break;}
                }
                const uint8_t *pxb=tess_detiled+4*((size_t)y*extent+x);
                const int is_ink=pxb[0]||pxb[1]||pxb[2];
                if(patch<0) {
                    if(is_ink)++t_foreign;
                    continue;
                }
                ++t_expected;
                if(!is_ink){++t_missing;continue;}
                /* The containing sub-triangle of the pixel's tessCoord: the
                 * equal-spacing grid cell it sits in, split by its diagonal. */
                const float level=patch==0?3.0f:1.0f;
                const float u=tess[0]*level,v=tess[1]*level;
                const float iu=floorf(u),iv=floorf(v);
                const int upright=((u-iu)+(v-iv))<=1.0f;
                float corner_tess[3][3];
                if(upright) {
                    corner_tess[0][0]=iu/level;corner_tess[0][1]=iv/level;
                    corner_tess[1][0]=(iu+1.0f)/level;corner_tess[1][1]=iv/level;
                    corner_tess[2][0]=iu/level;corner_tess[2][1]=(iv+1.0f)/level;
                } else {
                    corner_tess[0][0]=(iu+1.0f)/level;corner_tess[0][1]=iv/level;
                    corner_tess[1][0]=(iu+1.0f)/level;corner_tess[1][1]=(iv+1.0f)/level;
                    corner_tess[2][0]=iu/level;corner_tess[2][1]=(iv+1.0f)/level;
                }
                corner_tess[0][2]=1.0f-corner_tess[0][0]-corner_tess[0][1];
                corner_tess[1][2]=1.0f-corner_tess[1][0]-corner_tess[1][1];
                corner_tess[2][2]=1.0f-corner_tess[2][0]-corner_tess[2][1];
                /* The pixel's weights inside its sub-triangle, solved directly
                 * against its own tessCoord. */
                const float d=(corner_tess[1][0]-corner_tess[0][0])*
                    (corner_tess[2][1]-corner_tess[0][1])-
                    (corner_tess[2][0]-corner_tess[0][0])*
                    (corner_tess[1][1]-corner_tess[0][1]);
                if(d==0.0f){++t_wrong;++t_covered;continue;}
                const float wa=((corner_tess[1][0]-tess[0])*
                    (corner_tess[2][1]-corner_tess[0][1])-
                    (corner_tess[2][0]-tess[0])*
                    (corner_tess[1][1]-corner_tess[0][1]))/d;
                const float wb=((corner_tess[2][0]-corner_tess[0][0])*
                    (tess[1]-corner_tess[0][1])-
                    (tess[0]-corner_tess[0][0])*
                    (corner_tess[2][1]-corner_tess[0][1]))/d;
                const float weights[3]={wa,wb,1.0f-wa-wb};
                /* The evaluation half's colour at each corner: the quantised
                 * tessCoord components plus the patch's own constant. */
                float expect[3]={0,0,patch==0?0.125f:0.625f};
                for(int corner=0;corner<3;++corner)
                    for(int ch=0;ch<2;++ch) {
                        const float q=floorf(corner_tess[corner][ch]*level)/level;
                        expect[ch]+=weights[corner]*q;
                    }
                /* B8G8R8A8: byte 2 red, byte 1 green, byte 0 blue. */
                int wrong=0;
                for(int ch=0;ch<3;++ch) {
                    const unsigned got=pxb[ch];
                    const unsigned want=(unsigned)(expect[2-ch]*255.0f+0.5f);
                    const unsigned diff=got>want?got-want:want-got;
                    if(diff>1)++wrong;
                }
                if(wrong)++t_wrong;
                ++t_covered;
            }
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_PROBE rc=%d created=1 expected=%llu covered=%llu missing=%llu "
                "foreign=%llu wrong_color=%llu digest=%016llx patches=2 levels=3,1 "
                "stages_en=%08x ls_hs_config=%08x tf_param=%08x primitive=%08x",
                (int)tess_rc,(unsigned long long)t_expected,(unsigned long long)t_covered,
                (unsigned long long)t_missing,(unsigned long long)t_foreign,
                (unsigned long long)t_wrong,
                (unsigned long long)geometry_digest(tess_detiled,sizeof(tess_detiled)),
                t_stages_en,t_ls_hs,t_tf,
                tess_native->pair->uc.vgt_primitive_type.value);
            vkDestroyPipeline(d,tess_pipeline,NULL);
            vkDestroyCommandPool(d,tess_pool,NULL);
        } else {
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_TESS_PROBE rc=%d created=0 site=%u",(int)tess_rc,
                ps5vk_pipeline_refusal_site());
        }
    }
    for(unsigned i=0;i<4;++i)vkDestroyShaderModule(d,tess_modules[i],NULL);
tess_done:
#endif
#if PS5VK_GEOMETRY_ORDER_PROBE
    if(restart_vertex_buffer)vkDestroyBuffer(d,restart_vertex_buffer,NULL);
    if(restart_index_buffer)vkDestroyBuffer(d,restart_index_buffer,NULL);
    if(restart_vertex_memory)vkFreeMemory(d,restart_vertex_memory,NULL);
    if(restart_index_memory)vkFreeMemory(d,restart_index_memory,NULL);
    vkDestroyShaderModule(d,restart_vertex_module,NULL);
    vkDestroyShaderModule(d,restart_fragment_module,NULL);
#endif
    vkDestroyShaderModule(d,components_vertex_module,NULL);
    vkDestroyShaderModule(d,components_module,NULL);
    vkDestroyShaderModule(d,components_output_fragment_module,NULL);
    vkDestroyShaderModule(d,fragment_module,NULL);
    vkDestroyShaderModule(d,suppress_fragment_module,NULL);
    vkDestroyShaderModule(d,identity_vertex_module,NULL);
    vkDestroyPipelineLayout(d,layout,NULL);
    vkDestroyFramebuffer(d,fb,NULL);
    vkDestroyRenderPass(d,pass,NULL);
    vkDestroyImageView(d,view,NULL);
    vkUnmapMemory(d,memory);
    vkDestroyImage(d,image,NULL);
    vkFreeMemory(d,memory,NULL);
}
#endif
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
           props.maxMipLevels!=(q==2?15u:1u) || props.maxArrayLayers!=(q==2?PS5VK_MAX_IMAGE_ARRAY_LAYERS:1u) ||
           props.sampleCounts!=VK_SAMPLE_COUNT_1_BIT)
            fail("image-query-profile",-1);
        ps5log_printf(PS5LOG_MARK,"PS5VK_IMAGE_QUERY format=%u usage=%u max_width=%u max_height=%u max_bytes=%llu",
            query_formats[q],query_usages[q],props.maxExtent.width,props.maxExtent.height,
            (unsigned long long)props.maxResourceSize);
    }
#if PS5VK_INPUT_ATTACHMENT_PROBE
    {
        const VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageFormatProperties props;
        CHECK(vkGetPhysicalDeviceImageFormatProperties(physical,
            VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
            usage,0,&props));
        if(props.maxArrayLayers<PS5VK_INPUT_ATTACHMENT_LAYER_COUNT ||
           props.maxExtent.width<64 || props.maxExtent.height<64 ||
           props.maxExtent.depth!=1 || props.sampleCounts!=VK_SAMPLE_COUNT_1_BIT)
            fail("input-attachment-image-query",-1);
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_INPUT_ATTACHMENT_QUERY format=%u usage=%u max_layers=%u "
            "max_width=%u max_height=%u samples=%u",
            VK_FORMAT_R8G8B8A8_UNORM,usage,props.maxArrayLayers,
            props.maxExtent.width,props.maxExtent.height,props.sampleCounts);
    }
#endif
    if(PS5VK_IMAGE_TARGET) {
        VkImageFormatProperties props;
        VkImageType type=PS5VK_IMAGE_TARGET==3?VK_IMAGE_TYPE_3D:
            (PS5VK_IMAGE_TARGET>=4?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D);
        VkImageCreateFlags flags=PS5VK_IMAGE_TARGET==2?
            VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0;
        CHECK(vkGetPhysicalDeviceImageFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,
            type,VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,flags,&props));
        uint32_t expected_dimension=PS5VK_IMAGE_TARGET==2?PS5VK_MAX_IMAGE_CUBE:
            (PS5VK_IMAGE_TARGET==3?PS5VK_MAX_IMAGE_3D:
            (PS5VK_IMAGE_TARGET>=4?PS5VK_MAX_IMAGE_1D:PS5VK_MAX_IMAGE_2D));
        uint32_t expected_layers=(PS5VK_IMAGE_TARGET==1 || PS5VK_IMAGE_TARGET>=4)?
            PS5VK_MAX_IMAGE_ARRAY_LAYERS:(PS5VK_IMAGE_TARGET==2?6u:1u);
        if(props.maxExtent.width!=expected_dimension ||
           props.maxExtent.height!=(PS5VK_IMAGE_TARGET>=4?1u:expected_dimension) ||
           props.maxExtent.depth!=(PS5VK_IMAGE_TARGET==3?PS5VK_MAX_IMAGE_3D:1u) ||
           props.maxArrayLayers!=expected_layers)fail("layered-image-query",-1);
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_LAYERED_QUERY target=%s dimension=%u depth=%u layers=%u",
            layered_target_name(),
            props.maxExtent.width,props.maxExtent.depth,props.maxArrayLayers);
    }
    VkPhysicalDeviceProperties device_props;vkGetPhysicalDeviceProperties(physical,&device_props);
    /* maxViewports is bound to the multiViewport report: the Vulkan floor of
     * PS5VK_MULTI_VIEWPORT_COUNT when the feature is reported, exactly one
     * otherwise. A report of one without the feature is the shipping state;
     * either value with the other feature state is a profile inconsistency. */
    VkPhysicalDeviceFeatures device_features;vkGetPhysicalDeviceFeatures(physical,&device_features);
#if PS5VK_SAMPLE_RATE_PROBE
    /* The measurement reports the surface count the query path answers with
     * before the scene runs; the scene itself is dispatched after the device
     * exists, like every other probe. */
    if(!device_features.sampleRateShading)
        fail("sample-rate-diagnostic-feature",-1);
#endif
#if PS5VK_FRAGMENT_STORE_PROBE
    if(!device_features.fragmentStoresAndAtomics)
        fail("fragment-store-diagnostic-feature",-1);
#endif
    const uint32_t expected_viewports=device_features.multiViewport?
        (uint32_t)PS5VK_MULTI_VIEWPORT_COUNT:1u;
    if(device_props.limits.maxImageDimension1D<PS5VK_MAX_IMAGE_1D ||
       device_props.limits.maxImageDimension2D<1920 ||
       device_props.limits.maxImageDimension3D<PS5VK_MAX_IMAGE_3D ||
       device_props.limits.maxImageDimensionCube<PS5VK_MAX_IMAGE_CUBE ||
       device_props.limits.maxImageArrayLayers<PS5VK_MAX_IMAGE_ARRAY_LAYERS ||
       !ps5vk_graphics_vertex_bindings_available(&device_props.limits,
           PS5VK_GRAPHICS_SCISSOR_PROBE==13?16u:1u) ||
       device_props.limits.maxViewports!=expected_viewports || device_props.limits.maxColorAttachments!=1 ||
       /* Floors, not equalities. The sampled-descriptor limits were promoted to
        * the witnessed 16 per stage and 96 per set; pinning this gate to the
        * old single-descriptor profile made the graphics payload fail closed at
        * boot on hardware even though the device reports exactly what
        * ps5vk_physical_profile_valid requires of it. */
       device_props.limits.maxPerStageDescriptorSamplers<PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS ||
       device_props.limits.maxPerStageDescriptorSampledImages<PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS ||
       device_props.limits.maxDescriptorSetSamplers<PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS ||
       device_props.limits.maxDescriptorSetSampledImages<PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS ||
       !device_props.limits.maxSamplerAllocationCount)
        fail("graphics-limits-profile",-1);
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_LIMITS image_1d=%u image_2d=%u image_3d=%u image_cube=%u image_layers=%u framebuffer=%ux%u vertex_stride=%u bindings=%u viewports=%u",
        device_props.limits.maxImageDimension1D,device_props.limits.maxImageDimension2D,device_props.limits.maxImageDimension3D,
        device_props.limits.maxImageDimensionCube,device_props.limits.maxImageArrayLayers,
        device_props.limits.maxFramebufferWidth,
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
#if PS5VK_TESS_VARIANT==25 || PS5VK_TESS_VARIANT==55 || PS5VK_FRAGMENT_STORE_PROBE || PS5VK_DUAL_SOURCE_PROBE || PS5VK_TWO_MRT_PROBE || PS5VK_SAMPLE_RATE_PROBE
    VkPhysicalDeviceFeatures requested_features={0};
#if PS5VK_TESS_VARIANT==25 || PS5VK_TESS_VARIANT==55
    requested_features.drawIndirectFirstInstance=VK_TRUE;
#endif
#if PS5VK_FRAGMENT_STORE_PROBE
    requested_features.fragmentStoresAndAtomics=VK_TRUE;
#endif
#if PS5VK_DUAL_SOURCE_PROBE
    /* The witness exists to execute the SRC1 equation, so it must enable the
     * capability on the logical device.  The front end still refuses the
     * equation on a build whose platform does not report the feature, which is
     * the fail-closed evidence for every build that is not the measurement
     * one. */
    requested_features.dualSrcBlend=VK_TRUE;
#endif
#if PS5VK_TWO_MRT_PROBE
    /* The witness exists to write two colour attachments, which this profile
     * only serves when the independentBlend capability is enabled on the
     * logical device: the front end and the runtime compiler keep refusing the
     * two-target shape on a build whose platform does not report the feature,
     * which is the fail-closed evidence for every build that is not the
     * measurement one. */
    requested_features.independentBlend=VK_TRUE;
#endif
#if PS5VK_SAMPLE_RATE_PROBE
    /* The measurement scene exists to exercise the multisample path a device
     * reports, so it enables sampleRateShading on the logical device: the
     * platform bit is what makes the query report it, this is what makes
     * vkCreateDevice accept a request for it, and the front end refuses the
     * state on a build whose platform reports no such capability. */
    requested_features.sampleRateShading=VK_TRUE;
#endif
    di.pEnabledFeatures=&requested_features;
#endif
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
#if PS5VK_MULTIVIEW_VIEW_PROBE
    /* The witness is the whole run: one six-view scene, no other diagnostic. */
    multiview_view_probe(device);
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    if (PS5VK_SHELL_CLOSE)
        ps5log_line(PS5LOG_MARK,"PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1");
    ps5log_close("graphics-api-end");
    /* Termination belongs to Close Game, as in every other bounded run. */
    if (PS5VK_SHELL_CLOSE) for (;;) sleep(1);
    return 0;
#endif
#if PS5VK_INPUT_ATTACHMENT_PROBE
    /* One bounded two-subpass run.  The function returns success only after a
     * full deterministic GPU readback; no ordinary scene or presentation is
     * mixed into this artifact. */
    const struct ps5vk_input_attachment_probe_modules input_modules = {
        .vertex = ps5vk_runtime_input_attachment_vertex,
        .vertex_words = sizeof(ps5vk_runtime_input_attachment_vertex) / 4,
        .pattern_fragment = ps5vk_runtime_input_attachment_pattern,
        .pattern_fragment_words = sizeof(ps5vk_runtime_input_attachment_pattern) / 4,
        .transform_fragment = ps5vk_runtime_input_attachment_transform,
        .transform_fragment_words = sizeof(ps5vk_runtime_input_attachment_transform) / 4};
    CHECK(ps5vk_input_attachment_probe(device, &input_modules));
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    ps5log_close("graphics-api-end");
    return 0;
#endif
#if PS5VK_TWO_MRT_PROBE
    /* The witness is the whole run: one fragment module that writes two colour
     * attachments, drawn once into a two-target framebuffer, with BOTH targets
     * read back from that single draw and judged by the pure oracle before
     * anything is reported. */
    const struct ps5vk_two_mrt_probe_modules two_mrt_modules = {
        .vertex = ps5vk_runtime_vertex,
        .vertex_words = sizeof(ps5vk_runtime_vertex) / 4,
        .two_mrt_fragment = ps5vk_runtime_two_mrt_fragment,
        .two_mrt_fragment_words = sizeof(ps5vk_runtime_two_mrt_fragment) / 4};
    CHECK(ps5vk_two_mrt_probe(device, &two_mrt_modules));
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    ps5log_close("graphics-api-end");
    return 0;
#endif
#if PS5VK_SAMPLE_RATE_PROBE
    /* The measurement is the whole run: a multisampled colour target is
     * created, cleared through the native path, submitted, and read back from
     * its own storage. The oracle - one distinct 32-bit value across a span
     * that is the sample count times the single-sample footprint - cannot pass
     * on a surface sized for one sample. */
    {
        struct ps5vk_sample_rate_probe_params sample_rate_params = {
            .samples = VK_SAMPLE_COUNT_4_BIT, .extent = 64u,
            .clear = {0.25f, 0.5f, 0.75f, 1.0f}};
        CHECK(ps5vk_sample_rate_probe(device, &sample_rate_params));
    }
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    ps5log_close("graphics-api-end");
    return 0;
#endif
#if PS5VK_FRAGMENT_STORE_PROBE
    const struct ps5vk_fragment_store_probe_modules fragment_store_modules = {
        .vertex = ps5vk_runtime_input_attachment_vertex,
        .vertex_words = sizeof(ps5vk_runtime_input_attachment_vertex) / 4,
        .control_fragment = ps5vk_runtime_fragment_store_control,
        .control_fragment_words = sizeof(ps5vk_runtime_fragment_store_control) / 4,
        .atomic_fragment = ps5vk_runtime_fragment_store_atomic,
        .atomic_fragment_words = sizeof(ps5vk_runtime_fragment_store_atomic) / 4};
    CHECK(ps5vk_fragment_store_probe(device, &fragment_store_modules));
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    ps5log_close("graphics-api-end");
    return 0;
#endif
#if PS5VK_DUAL_SOURCE_PROBE
    /* The witness is the whole run: one fragment module that exports both
     * sources of attachment zero, drawn once with blending disabled and once
     * with the accepted SRC1 equation, each judged on its own readback before
     * anything is reported. */
    const struct ps5vk_dual_source_probe_modules dual_source_modules = {
        .vertex = ps5vk_runtime_vertex,
        .vertex_words = sizeof(ps5vk_runtime_vertex) / 4,
        .dual_fragment = ps5vk_runtime_dual_source_fragment,
        .dual_fragment_words = sizeof(ps5vk_runtime_dual_source_fragment) / 4};
    CHECK(ps5vk_dual_source_probe(device, &dual_source_modules));
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    ps5log_close("graphics-api-end");
    return 0;
#endif
#if PS5VK_CLIP_CULL_PROBE
    /* The witness is the whole run: seven distance cases, no other diagnostic
     * and no presentation. Every case is judged on the GPU's own readback
     * before the next one is recorded. */
    clip_cull_probe(device);
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    if (PS5VK_SHELL_CLOSE)
        ps5log_line(PS5LOG_MARK,"PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1");
    ps5log_close("graphics-api-end");
    if (PS5VK_SHELL_CLOSE) for (;;) sleep(1);
    return 0;
#endif
#if PS5VK_GEOMETRY_PROBE
    /* The geometry witness is the whole run: five coverage cases, no other
     * diagnostic and no presentation. */
    geometry_probe(device);
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    ps5log_line(PS5LOG_MARK,"PS5VK_GRAPHICS_API_CLEANUP_COMPLETE");
    if (PS5VK_SHELL_CLOSE)
        ps5log_line(PS5LOG_MARK,"PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1");
    ps5log_close("graphics-api-end");
    if (PS5VK_SHELL_CLOSE) for (;;) sleep(1);
    return 0;
#endif
    VkAttachmentDescription attachment = {.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=PS5VK_GRAPHICS_SCENE?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference colorref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    /* The witness LOADS depth so the render pass contributes nothing to its
     * contents; whatever the depth test reads came from the explicit clear. */
    VkAttachmentDescription attachments[2]={attachment,{.format=VK_FORMAT_D32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=PS5VK_GRAPHICS_SCISSOR_PROBE==14?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=PS5VK_GRAPHICS_SCISSOR_PROBE==14?
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference depthref={1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&colorref};
    int use_depth=graphics_library.programs[0].key.descriptor_set_count!=0;
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
    use_depth=0;
#endif
    if(use_depth)subpass.pDepthStencilAttachment=&depthref;
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=use_depth?2:1,.pAttachments=attachments,
        .subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass pass; CHECK(vkCreateRenderPass(device,&ri,NULL,&pass));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const struct ps5vk_graphics_key *key=&graphics_library.programs[0].key;
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
    VkVertexInputBindingDescription runtime_binding={0,
        PS5VK_GRAPHICS_SCISSOR_PROBE==12?24:16,VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription runtime_attributes[2]={
        {0,0,PS5VK_GRAPHICS_SCISSOR_PROBE==12?VK_FORMAT_R32G32B32_SFLOAT:
            VK_FORMAT_R32G32B32A32_SINT,0},
        {1,0,VK_FORMAT_R32G32B32_SFLOAT,12}};
    struct ps5vk_set_signature runtime_sampled={0};
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==12) {
        runtime_sampled.count=1;runtime_sampled.binding[0].count=1;
        runtime_sampled.binding[0].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
        runtime_sampled.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        for(unsigned binding_index=1;binding_index<PS5VK_MAX_BINDINGS;++binding_index)
            runtime_sampled.binding[binding_index].first=1;
    }
    struct ps5vk_graphics_key runtime_key={
        .vertex={.words=PS5VK_GRAPHICS_SCISSOR_PROBE==12?ps5vk_runtime_mipmap_vertex:
                (PS5VK_GRAPHICS_SCISSOR_PROBE==8?ps5vk_runtime_vertex_sint:ps5vk_runtime_vertex),
            .word_count=PS5VK_GRAPHICS_SCISSOR_PROBE==12?sizeof(ps5vk_runtime_mipmap_vertex)/4:
                (PS5VK_GRAPHICS_SCISSOR_PROBE==8?sizeof(ps5vk_runtime_vertex_sint)/4:sizeof(ps5vk_runtime_vertex)/4),.entry="main"},
        .fragment={.words=PS5VK_GRAPHICS_SCISSOR_PROBE==12?ps5vk_runtime_texture_fragment:
                (PS5VK_GRAPHICS_SCISSOR_PROBE==8?ps5vk_runtime_vertex_format_fragment:ps5vk_runtime_fragment),
            .word_count=PS5VK_GRAPHICS_SCISSOR_PROBE==12?sizeof(ps5vk_runtime_texture_fragment)/4:
                (PS5VK_GRAPHICS_SCISSOR_PROBE==8?sizeof(ps5vk_runtime_vertex_format_fragment)/4:sizeof(ps5vk_runtime_fragment)/4),.entry="main"},
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15},
        .vertex_binding_count=(PS5VK_GRAPHICS_SCISSOR_PROBE==8 || PS5VK_GRAPHICS_SCISSOR_PROBE==12)?1:0,
        .vertex_attribute_count=PS5VK_GRAPHICS_SCISSOR_PROBE==12?2:(PS5VK_GRAPHICS_SCISSOR_PROBE==8?1:0),
        .vertex_bindings=(PS5VK_GRAPHICS_SCISSOR_PROBE==8 || PS5VK_GRAPHICS_SCISSOR_PROBE==12)?&runtime_binding:NULL,
        .vertex_attributes=(PS5VK_GRAPHICS_SCISSOR_PROBE==8 || PS5VK_GRAPHICS_SCISSOR_PROBE==12)?runtime_attributes:NULL,
        .descriptor_set_count=PS5VK_GRAPHICS_SCISSOR_PROBE==12?1:0,
        .descriptor_sets=PS5VK_GRAPHICS_SCISSOR_PROBE==12?&runtime_sampled:NULL};
    VkVertexInputBindingDescription multiple_bindings[16];
    VkVertexInputAttributeDescription multiple_attributes[16];
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==13) {
        for(unsigned location=0;location<16;++location) {
            unsigned b=15-location;
            multiple_bindings[location]=(VkVertexInputBindingDescription){b,48+4*b,VK_VERTEX_INPUT_RATE_VERTEX};
            multiple_attributes[location]=(VkVertexInputAttributeDescription){location,b,VK_FORMAT_R32G32B32A32_SFLOAT,4*(b%3)};
        }
        runtime_key.vertex=(struct ps5vk_graphics_module_key){
            .words=ps5vk_runtime_vertex_bindings,.word_count=sizeof(ps5vk_runtime_vertex_bindings)/4,.entry="main"};
        runtime_key.fragment=(struct ps5vk_graphics_module_key){
            .words=ps5vk_runtime_vertex_format_fragment,.word_count=sizeof(ps5vk_runtime_vertex_format_fragment)/4,.entry="main"};
        runtime_key.vertex_binding_count=runtime_key.vertex_attribute_count=16;
        runtime_key.vertex_bindings=multiple_bindings;runtime_key.vertex_attributes=multiple_attributes;
    }
    key=&runtime_key;
    const struct ps5vk_graphics_program *unexpected=NULL;
    if(ps5vk_graphics_resolve(&graphics_library,key,&unexpected)!=VK_ERROR_FEATURE_NOT_PRESENT)
        fail("runtime-graphics-not-independent",-1);
    ps5log_line(PS5LOG_MARK,"PS5VK_RUNTIME_GRAPHICS_INPUT absent_from_offline_library=1");
#endif
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
    const struct ps5vk_graphics_module_key *modules[4]={&key->vertex,&key->fragment,NULL,NULL};
    unsigned shader_count=2,fragment_shader=1;
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
    const struct ps5vk_graphics_module_key vertex_sint={
        .words=ps5vk_runtime_vertex_sint,.word_count=sizeof(ps5vk_runtime_vertex_sint)/4,.entry="main"};
    const struct ps5vk_graphics_module_key vertex_uint={
        .words=ps5vk_runtime_vertex_uint,.word_count=sizeof(ps5vk_runtime_vertex_uint)/4,.entry="main"};
    const struct ps5vk_graphics_module_key vertex_unorm={
        .words=ps5vk_runtime_vertex_unorm,.word_count=sizeof(ps5vk_runtime_vertex_unorm)/4,.entry="main"};
    const struct ps5vk_graphics_module_key vertex_format_fragment={
        .words=ps5vk_runtime_vertex_format_fragment,
        .word_count=sizeof(ps5vk_runtime_vertex_format_fragment)/4,.entry="main"};
    if(PS5VK_GRAPHICS_SCISSOR_PROBE==8) {
        modules[0]=&vertex_sint;modules[1]=&vertex_uint;modules[2]=&vertex_unorm;
        modules[3]=&vertex_format_fragment;shader_count=4;fragment_shader=3;
    }
#endif
    VkShaderModule shaders[4]; VkPipelineShaderStageCreateInfo stages[2];
    for (unsigned j=0;j<shader_count;++j) {
        VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=modules[j]->word_count*4,.pCode=modules[j]->words};
        CHECK(vkCreateShaderModule(device,&si,NULL,&shaders[j]));
    }
    stages[0]=(VkPipelineShaderStageCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=shaders[0],.pName="main"};
    stages[1]=(VkPipelineShaderStageCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=shaders[fragment_shader],.pName="main"};
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
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
    VkSpecializationMapEntry component_entries[4]={{0,0,sizeof(uint32_t)},
        {1,sizeof(uint32_t),sizeof(uint32_t)},
        {2,2*sizeof(uint32_t),sizeof(uint32_t)},
        {3,3*sizeof(uint32_t),sizeof(uint32_t)}};
    union ps5vk_vertex_probe_expected expected_components;
    VkSpecializationInfo component_specialization={4,component_entries,
        sizeof(expected_components),&expected_components};
    uint32_t binding_mode=0;
    VkSpecializationMapEntry binding_entry={0,0,sizeof(binding_mode)};
    VkSpecializationInfo binding_specialization={1,&binding_entry,sizeof(binding_mode),&binding_mode};
#endif
    for (unsigned iteration=0;iteration<diagnostic_iterations();++iteration) {
        unsigned witness_index=PS5VK_GRAPHICS_WITNESSES==2 && iteration==3?5:iteration;
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
        if(PS5VK_GRAPHICS_SCISSOR_PROBE==13) {
            binding_mode=iteration%3;
            stages[0].pSpecializationInfo=&binding_specialization;
        }
        if(PS5VK_GRAPHICS_SCISSOR_PROBE==8) {
            struct ps5vk_vertex_format_case c;
            if(ps5vk_vertex_format_case(witness_index,&c))fail("vertex-format-case",-1);
            runtime_binding.stride=c.bytes;
            runtime_attributes[0].format=c.format;
            memcpy(&expected_components,&c.expected,sizeof(expected_components));
            for(unsigned j=0;j<4;++j)component_entries[j].offset=j*sizeof(uint32_t);
            stages[0].module=shaders[c.numeric==PS5VK_VERTEX_PROBE_SINT?0:
                (c.numeric==PS5VK_VERTEX_PROBE_UINT?1:2)];
            stages[0].pSpecializationInfo=&component_specialization;
        }
#endif
#if PS5VK_GRAPHICS_DRAW
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_COMPUTE_CONTROL phase=before-graphics iteration=%u",iteration);
        ps5vk_compute_regression(device);
#endif
        if(PS5VK_GRAPHICS_WITNESSES) {
            depth.depthTestEnable=PS5VK_GRAPHICS_WITNESSES==2?VK_FALSE:VK_TRUE;
            viewport.width=1920;viewport.height=1080;
            scissor=(VkRect2D){{(int32_t)ps5vk_scene_witnesses[witness_index].x,(int32_t)ps5vk_scene_witnesses[witness_index].y},{1,1}};
        } else if(PS5VK_GRAPHICS_SCISSOR_PROBE==13 || ps5vk_scene_full_frame_probe(PS5VK_GRAPHICS_SCISSOR_PROBE)) {
            depth.depthTestEnable=VK_FALSE;
            scissor=PS5VK_GRAPHICS_SCISSOR_PROBE==6 && witness_index>=6 ?
                (VkRect2D){{960,540},{1,1}}:(VkRect2D){{0,0},{1920,1080}};
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
    for (unsigned j=0;j<shader_count;++j) vkDestroyShaderModule(device,shaders[j],NULL);
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

#ifndef PS5VK_READBACK_COMMANDS_PS5_H
#define PS5VK_READBACK_COMMANDS_PS5_H
#include "vk_command.h"
#include "image_layout_state.h"
#include "color_detile.h"
#include "depth_detile.h"
#include "vk_image.h"
#include "color_attachment_contract.h"
#include "color_barrier.h"
#include "vk_image_transfer.h"
#include "depth_layout.h"
#include "readback_region.h"

/* `aspect` is zero for a colour or depth-only surface and names the one
 * aspect a copy reads from the combined depth/stencil attachment. */
struct ps5vk_readback_plan {
    VkImage image; VkBuffer buffer; VkDeviceSize layer_stride;
    VkImageAspectFlags aspect;
    /* Set for a general colour region (ps5vk_readback_regions_commands): the
     * completion path detiles exactly that region instead of the whole surface. */
    VkBool32 region_copy;
    VkBufferImageCopy region;
};
/* The most image-to-buffer copies one readback may carry: every colour target
 * of a subpass, or the regions of a general colour readback. */
enum { PS5VK_MAX_READBACK_REGIONS = 8 };

/* The render-pass module reads back every attachment its pass rendered, in one
 * command buffer: one handover barrier and one whole-surface copy per target,
 * followed by the buffer scope that publishes them. The single-copy shapes
 * above belong to the draw module, which reads one surface back; this set
 * carries as many targets as a subpass may name, and each target is its own
 * surface and its own buffer. */
struct ps5vk_readback_set {
    struct ps5vk_readback_plan target[PS5VK_MAX_READBACK_REGIONS];
    unsigned count;
};

struct ps5vk_readback_partition {
    unsigned prefix_count;
    unsigned readback_first;
    unsigned readback_count;
    unsigned suffix_first;
    unsigned suffix_count;
};

/* A render-pass postlude may order unrelated shader writes before the fixed
 * image readback sequence.  Locate that sequence by its single image-to-buffer
 * copy, whose immediately preceding operation must be its image barrier.  The
 * strict readback validator below still owns the complete four-operation
 * readback. The texture renderer may append its exact TRANSFER_SRC-to-colour
 * handback after that readback; return it as a separate suffix so the caller
 * can run it through the ordinary upload/barrier validator in the same serial. */
static inline VkResult ps5vk_readback_partition(
    const struct ps5vk_operation *ops,unsigned count,
    struct ps5vk_readback_partition *out)
{
    if(!ops || !count || !out)return VK_ERROR_FEATURE_NOT_PRESENT;
    unsigned copy=count,copies=0;
    for(unsigned i=0;i<count;++i)
        if(ops[i].type==PS5VK_COPY_IMAGE_BUFFER){copy=i;++copies;}
    if(copies!=1 || !copy || count-copy<3)return VK_ERROR_FEATURE_NOT_PRESENT;
    const unsigned first=copy-1;
    if(ops[first].type!=PS5VK_IMAGE_BARRIER)return VK_ERROR_FEATURE_NOT_PRESENT;
    const unsigned readback_end=first+4;
    if(readback_end>count)return VK_ERROR_FEATURE_NOT_PRESENT;
    const unsigned suffix_count=count-readback_end;
    if(suffix_count) {
        if(suffix_count!=1 || ops[readback_end].type!=PS5VK_IMAGE_BARRIER)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        const struct ps5vk_operation *op=&ops[readback_end];
        const VkImageMemoryBarrier *b=&op->image_barrier;
        if(b->image!=ops[first].image_barrier.image ||
           b->oldLayout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           b->newLayout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
           b->srcAccessMask!=VK_ACCESS_TRANSFER_READ_BIT ||
           b->dstAccessMask!=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT ||
           op->src_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT ||
           op->dst_stage!=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    *out=(struct ps5vk_readback_partition){
        .prefix_count=first,.readback_first=first,.readback_count=4,
        .suffix_first=readback_end,.suffix_count=suffix_count};
    return VK_SUCCESS;
}

/* The same bounded full-color readback may follow a render pass or be a
 * separate submission. This only validates and stages the layout: the caller
 * must flush CB/DB caches and observe its exact GPU serial before detiling or
 * publishing host bytes. A plan is not evidence of a completed transfer. */
/* One aspect of the combined depth/stencil attachment read back into a
 * buffer. Two recorded shapes, like the single-aspect readback below:
 *
 *   staged    one image barrier that hands the aspect (or both) to
 *             TRANSFER_SRC, the copy of one aspect, the host barrier and the
 *             aggregate the recorder appends;
 *   unstaged  the copy, the host barrier and the aggregate, for an aspect an
 *             earlier submission already moved to TRANSFER_SRC.
 *
 * The copied aspect must be in TRANSFER_SRC in this transaction - checked per
 * aspect, so the other aspect may be anywhere - and the barrier moves only
 * the aspects it names. Nothing is staged on refusal. */
static inline VkResult ps5vk_depth_stencil_readback_commands(VkDevice d,
    const struct ps5vk_operation *ops,unsigned count,
    struct ps5vk_layout_state *layouts,struct ps5vk_readback_plan *out)
{
    if(!d || !ops || !layouts || !out || (count!=4 && count!=3))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const int staged=count==4;
    const struct ps5vk_operation *copy=&ops[staged?1:0];
    const struct ps5vk_operation *host=&ops[staged?2:1];
    const struct ps5vk_operation *aggregate=&ops[staged?3:2];
    if((staged && ops[0].type!=PS5VK_IMAGE_BARRIER) || copy->type!=PS5VK_COPY_IMAGE_BUFFER ||
       host->type!=PS5VK_BARRIER || aggregate->type!=PS5VK_BARRIER)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImage image=copy->copy_image;
    const VkBufferImageCopy *r=&copy->copy_region;
    const VkImageAspectFlags aspect=r->imageSubresource.aspectMask;
    const uint64_t texel=aspect==VK_IMAGE_ASPECT_STENCIL_BIT?1u:4u;
    if(!ps5vk_depth_stencil_attachment_image(image) || image->device!=d ||
       !(image->info.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
       (aspect!=VK_IMAGE_ASPECT_DEPTH_BIT && aspect!=VK_IMAGE_ASPECT_STENCIL_BIT) ||
       copy->copy_layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || !copy->copy_destination ||
       r->bufferOffset || (r->bufferRowLength && r->bufferRowLength!=image->info.extent.width) ||
       (r->bufferImageHeight && r->bufferImageHeight!=image->info.extent.height) ||
       r->imageSubresource.mipLevel || r->imageSubresource.baseArrayLayer ||
       r->imageSubresource.layerCount!=1 || r->imageOffset.x || r->imageOffset.y ||
       r->imageOffset.z || r->imageExtent.width!=image->info.extent.width ||
       r->imageExtent.height!=image->info.extent.height || r->imageExtent.depth!=1 ||
       host->buffer_barrier.buffer!=copy->copy_destination ||
       host->src_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT || host->dst_stage!=VK_PIPELINE_STAGE_HOST_BIT ||
       host->src_access!=VK_ACCESS_TRANSFER_WRITE_BIT || host->dst_access!=VK_ACCESS_HOST_READ_BIT ||
       aggregate->buffer_barrier.buffer || aggregate->src_access || aggregate->dst_access ||
       aggregate->src_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT ||
       aggregate->dst_stage!=VK_PIPELINE_STAGE_HOST_BIT)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint64_t pixels=(uint64_t)image->info.extent.width*image->info.extent.height;
    struct ps5vk_depth_stencil_layout planes;
    void *source,*destination;VkDeviceSize source_bytes,destination_bytes;
    if(ps5vk_depth_stencil_layout(image->info.extent.width,image->info.extent.height,&planes) ||
       ps5vk_image_span(d,image,&source,&source_bytes)!=VK_SUCCESS ||
       ps5vk_buffer_span(d,copy->copy_destination,0,VK_WHOLE_SIZE,&destination,
           &destination_bytes)!=VK_SUCCESS ||
       source_bytes<planes.bytes || destination_bytes<pixels*texel ||
       (host->buffer_barrier.size!=VK_WHOLE_SIZE && host->buffer_barrier.size<pixels*texel))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uintptr_t src=(uintptr_t)source,dst=(uintptr_t)destination;
    if(src<dst ? source_bytes>dst-src : pixels*texel>src-dst)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_layout_state updated=*layouts;
    if(staged) {
        const VkImageMemoryBarrier *b=&ops[0].image_barrier;
        if(b->image!=image || !ps5vk_depth_stencil_barrier(b,VK_TRUE,VK_TRUE) ||
           !(b->subresourceRange.aspectMask&aspect) ||
           b->newLayout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           !(b->dstAccessMask&VK_ACCESS_TRANSFER_READ_BIT) ||
           !(ops[0].dst_stage&VK_PIPELINE_STAGE_TRANSFER_BIT))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        VkResult rc=ps5vk_layout_transition_aspects(&updated,image,
            b->subresourceRange.aspectMask,b->oldLayout,b->newLayout);
        if(rc!=VK_SUCCESS)return rc;
    }
    VkResult rc=ps5vk_layout_require_aspects(&updated,image,aspect,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if(rc!=VK_SUCCESS)return rc;
    *layouts=updated;
    *out=(struct ps5vk_readback_plan){.image=image,.buffer=copy->copy_destination,
        .layer_stride=planes.bytes,.aspect=aspect};
    return VK_SUCCESS;
}

/* Caller must have observed the exact GPU completion serial and invalidated
 * the source. One aspect's plane, detiled with its own 64KB_Z_X equation. */
static inline int ps5vk_depth_stencil_readback_detile(VkImage image,
    VkImageAspectFlags aspect,void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes)
{
    struct ps5vk_depth_stencil_layout planes;
    if(!ps5vk_depth_stencil_attachment_image(image) || !source ||
       ps5vk_depth_stencil_layout(image->info.extent.width,image->info.extent.height,&planes) ||
       source_bytes<planes.bytes)return -1;
    const uint32_t w=image->info.extent.width,h=image->info.extent.height;
    if(aspect==VK_IMAGE_ASPECT_DEPTH_BIT)
        return ps5vk_depth_64k_zx_detile(destination,destination_bytes,source,
            (size_t)planes.depth.bytes,w,h);
    if(aspect==VK_IMAGE_ASPECT_STENCIL_BIT)
        return ps5vk_stencil_64k_zx_detile(destination,destination_bytes,
            (const unsigned char *)source+planes.stencil_offset,(size_t)planes.stencil_bytes,w,h);
    return -1;
}

static inline VkResult ps5vk_readback_commands(VkDevice d,
    const struct ps5vk_operation *ops,unsigned count,VkImage color,
    struct ps5vk_layout_state *layouts,struct ps5vk_readback_plan *out,
    unsigned *failure_site)
{
    /* The combined depth/stencil attachment has its own per-aspect shape. */
    if(ops && (count==3 || count==4) &&
       ps5vk_depth_stencil_attachment_image(
           ops[count==4?1:0].type==PS5VK_COPY_IMAGE_BUFFER?ops[count==4?1:0].copy_image:NULL)) {
        if(color){if(failure_site)*failure_site=12;return VK_ERROR_FEATURE_NOT_PRESENT;}
        VkResult rc=ps5vk_depth_stencil_readback_commands(d,ops,count,layouts,out);
        if(rc!=VK_SUCCESS && failure_site)*failure_site=13;
        return rc;
    }
#define READBACK_REFUSE(site) do { if(failure_site)*failure_site=(site); return VK_ERROR_FEATURE_NOT_PRESENT; } while(0)
    /* Two recorded shapes. The colour readback transitions its attachment
     * itself, so it is four operations: the image barrier, the copy, the host
     * barrier and the aggregate. A DEPTH readback is submitted on its own,
     * AFTER the render submission already moved the surface to
     * TRANSFER_SRC_OPTIMAL, so it records only the copy and the host barrier
     * (vktDrawImageObjectUtil.cpp Image::readUsingBuffer, whose leading
     * transition is skipped for any layout but UNDEFINED). Its host barrier
     * still records the buffer entry AND the aggregate the recorder appends
     * for a call with no image barrier, so the unstaged shape is three
     * operations, not two. Nothing else about the readback differs, so the two
     * shapes share every check below. */
    if(!d || !ops || !layouts || !out || (count!=5 && count!=4 && count!=3))
        READBACK_REFUSE(1);
    const int staged=count==5 || count==4;
    const int restore=count==5;
    if(staged ? (ops[0].type!=PS5VK_IMAGE_BARRIER || ops[1].type!=PS5VK_COPY_IMAGE_BUFFER ||
                 ops[2].type!=PS5VK_BARRIER || ops[3].type!=PS5VK_BARRIER ||
                 (restore && ops[4].type!=PS5VK_IMAGE_BARRIER))
              : (ops[0].type!=PS5VK_COPY_IMAGE_BUFFER || ops[1].type!=PS5VK_BARRIER ||
                 ops[2].type!=PS5VK_BARRIER))
        READBACK_REFUSE(2);
    const VkImageMemoryBarrier *b=staged?&ops[0].image_barrier:NULL;
    const struct ps5vk_operation *copy=staged?&ops[1]:&ops[0];
    const struct ps5vk_operation *host=staged?&ops[2]:&ops[1];
    const struct ps5vk_operation *aggregate=staged?&ops[3]:&ops[2];
    VkImage image=staged?b->image:copy->copy_image;
    /* The same four-operation readback over a DEPTH attachment. Its aspect,
     * its attachment layout and the stage that wrote it are depth's, and the
     * recorder already admits exactly this shape for a D32 image that declares
     * the transfer-source role (src/vk_transfer.c). Everything else about the
     * readback - the copy, the host barrier and the aggregate - is identical,
     * so only the parts that name colour are switched here. */
    const int depth_source=ps5vk_depth_readback_image(image);
    /* The unstaged shape belongs to a surface the previous submission already
     * handed to its readback, which the pinned helper does for both aspects:
     * a depth attachment, and a colour attachment that also declares a
     * transfer destination and was cleared through it. */
    if(!staged && !depth_source && !ps5vk_colour_transfer_image(image))
        READBACK_REFUSE(3);
    if(!image || image->device!=d || (color && image!=color) ||
       (!staged ? 0 : depth_source ?
        (b->oldLayout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
         b->srcAccessMask!=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT ||
         !(ops[0].src_stage & (VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
                                                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT|
                                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) ||
         (ops[0].src_stage & ~(VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
                                                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT|
                                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT))) :
        ((b->oldLayout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
          !(ps5vk_array_color_image(image) && b->oldLayout==VK_IMAGE_LAYOUT_GENERAL) &&
          !(ps5vk_basic_colour_readback_image(image) && b->oldLayout==VK_IMAGE_LAYOUT_GENERAL)) ||
         b->srcAccessMask!=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT ||
         (ops[0].src_stage!=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
          ops[0].src_stage!=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT))) ||
       (staged && (b->newLayout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
                   b->dstAccessMask!=VK_ACCESS_TRANSFER_READ_BIT ||
                   ops[0].dst_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT)) ||
       copy->copy_image!=image || copy->copy_layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
       !copy->copy_destination || host->buffer_barrier.buffer!=copy->copy_destination ||
       host->src_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT || host->dst_stage!=VK_PIPELINE_STAGE_HOST_BIT ||
       host->src_access!=VK_ACCESS_TRANSFER_WRITE_BIT || host->dst_access!=VK_ACCESS_HOST_READ_BIT ||
       (aggregate->buffer_barrier.buffer || aggregate->src_access ||
        aggregate->dst_access ||
        aggregate->src_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT ||
        aggregate->dst_stage!=VK_PIPELINE_STAGE_HOST_BIT))
        READBACK_REFUSE(4);
    if(restore) {
        const VkImageMemoryBarrier *restore_barrier=&ops[4].image_barrier;
        if(restore_barrier->image!=image ||
           restore_barrier->oldLayout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           restore_barrier->newLayout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
           restore_barrier->srcAccessMask!=VK_ACCESS_TRANSFER_READ_BIT ||
           restore_barrier->dstAccessMask!=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT ||
           ops[4].src_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT ||
           ops[4].dst_stage!=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)
            READBACK_REFUSE(10);
    }
    const VkBufferImageCopy *r=&copy->copy_region;
    const VkImageCreateInfo *i=&image->info;
    const VkImageUsageFlags required=(depth_source?
        (VkImageUsageFlags)VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
        (VkImageUsageFlags)VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    uint64_t plane=(uint64_t)i->extent.width*i->extent.height;
    if(!i->arrayLayers || plane>SIZE_MAX/4/i->arrayLayers)READBACK_REFUSE(5);
    uint64_t pixels=plane*i->arrayLayers;
    /* A colour readback reads a 32-bit-per-texel surface: the normalized
     * attachment this profile has always read back, and - only in the build
     * that serves it - the integer one its independentBlend oracle uses. The
     * bytes are tiled by the same 64KB_R_X equation either way. */
    if(!(depth_source ? i->format==VK_FORMAT_D32_SFLOAT :
          (i->format==VK_FORMAT_R8G8B8A8_UNORM ||
           ps5vk_color_target_integer_served(i->format))) ||
       i->samples!=VK_SAMPLE_COUNT_1_BIT ||
       i->mipLevels!=1 || (i->arrayLayers!=1 && !ps5vk_array_color_image(image)) || i->extent.depth!=1 ||
       (i->usage&required)!=required || !pixels || pixels>SIZE_MAX/4 ||
       r->bufferOffset || (r->bufferRowLength && r->bufferRowLength!=i->extent.width) ||
       (r->bufferImageHeight && r->bufferImageHeight!=i->extent.height) ||
       r->imageSubresource.aspectMask!=(depth_source?
           (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT:
           (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT) ||
       r->imageSubresource.mipLevel || r->imageSubresource.baseArrayLayer ||
       r->imageSubresource.layerCount!=i->arrayLayers || r->imageOffset.x || r->imageOffset.y || r->imageOffset.z ||
       r->imageExtent.width!=i->extent.width || r->imageExtent.height!=i->extent.height ||
       r->imageExtent.depth!=1 || host->buffer_barrier.offset ||
       (host->buffer_barrier.size!=VK_WHOLE_SIZE && host->buffer_barrier.size<pixels*4))
        READBACK_REFUSE(6);
    void *source,*destination;VkDeviceSize source_bytes,destination_bytes;
    /* The tiled footprint follows the surface's own addressing: 64KB_Z_X for a
     * depth surface, 64KB_R_X for a colour one. Sizing a depth surface with the
     * colour equation would compare its bytes against the wrong footprint. */
    size_t tiled=depth_source?
        ps5vk_depth_64k_zx_surface_size(i->extent.width,i->extent.height):
        ps5vk_color_64k_rx_surface_size(4,i->extent.width,i->extent.height);
    if(tiled==SIZE_MAX ||
       ps5vk_image_span(d,image,&source,&source_bytes)!=VK_SUCCESS ||
       ps5vk_buffer_span(d,copy->copy_destination,0,VK_WHOLE_SIZE,&destination,&destination_bytes)!=VK_SUCCESS ||
       source_bytes<tiled || destination_bytes<pixels*4)
        READBACK_REFUSE(7);
    /* Native array allocations consist of equally-sized, 128 KiB-aligned
     * layer footprints. Never infer a tight width*height stride for tiled data. */
    VkDeviceSize stride=source_bytes/i->arrayLayers;
    if(source_bytes%i->arrayLayers || stride<tiled ||
       (i->arrayLayers>1 && stride%131072u))READBACK_REFUSE(8);
    uintptr_t src=(uintptr_t)source,dst=(uintptr_t)destination;
    if(src<dst ? source_bytes>dst-src : pixels*4>src-dst)READBACK_REFUSE(9);
    /* The unstaged shape carries no transition of its own: the surface is
     * already where the copy needs it, so there is nothing to stage. */
    if(staged) {
        struct ps5vk_layout_state updated=*layouts;
        VkResult rc=ps5vk_layout_transition(&updated,image,b->oldLayout,b->newLayout);
        if(rc!=VK_SUCCESS){if(failure_site)*failure_site=11;return rc;}
        if(restore) {
            const VkImageMemoryBarrier *restore_barrier=&ops[4].image_barrier;
            rc=ps5vk_layout_transition(&updated,image,restore_barrier->oldLayout,
                restore_barrier->newLayout);
            if(rc!=VK_SUCCESS){if(failure_site)*failure_site=11;return rc;}
        }
        *layouts=updated;
    }
    *out=(struct ps5vk_readback_plan){.image=image,.buffer=copy->copy_destination,
        .layer_stride=stride};
    return VK_SUCCESS;
#undef READBACK_REFUSE
}

/* The multi-target form: the pinned render-pass module's own readback command
 * buffer. pushReadImagesToBuffers (vktRenderPassTests.cpp:3582) records ONE
 * barrier call carrying the identity handover of every attachment it reads
 * back, then one whole-surface copy per attachment in attachment order, then
 * the buffer scope that publishes those copies. Each target is a separate
 * colour attachment of the same readback role, so every per-copy check the
 * single shape applies applies here too, once per target. */
static inline VkResult ps5vk_readback_commands_set(VkDevice d,
    const struct ps5vk_operation *ops,unsigned count,VkImage color,
    struct ps5vk_layout_state *layouts,struct ps5vk_readback_set *out)
{
    if(!d || !ops || !layouts || !out || count<3)return VK_ERROR_FEATURE_NOT_PRESENT;
    memset(out,0,sizeof(*out));
    unsigned at=0;
    /* The handover of every target, in attachment order. */
    while(at<count && ops[at].type==PS5VK_IMAGE_BARRIER) {
        const VkImageMemoryBarrier *b=&ops[at].image_barrier;
        VkImage image=b->image;
        if(out->count==PS5VK_MAX_COLOR_ATTACHMENTS ||
           !ps5vk_colour_readback_handover_barrier(b) ||
           !image || image->device!=d || (color && image!=color) ||
           !ps5vk_colour_transfer_image(image))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        for(unsigned k=0;k<out->count;++k)
            if(out->target[k].image==image)return VK_ERROR_FEATURE_NOT_PRESENT;
        out->target[out->count].image=image;
        out->target[out->count].buffer=VK_NULL_HANDLE;
        out->target[out->count].layer_stride=0;
        ++out->count;
        if(ps5vk_layout_transition(layouts,image,b->oldLayout,b->newLayout)!=VK_SUCCESS)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        ++at;
    }
    if(!out->count)return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Then exactly one whole-surface copy per handed-over attachment, in the
     * same order. */
    const unsigned targets=out->count;
    for(unsigned t=0;t<targets;++t) {
        if(at>=count || ops[at].type!=PS5VK_COPY_IMAGE_BUFFER)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        const struct ps5vk_operation *copy=&ops[at];
        VkImage image=out->target[t].image;
        if(copy->copy_image!=image ||
           copy->copy_layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           !copy->copy_destination)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        const VkImageCreateInfo *i=&image->info;
        const VkBufferImageCopy *r=&copy->copy_region;
        const VkImageUsageFlags required=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        uint64_t plane=(uint64_t)i->extent.width*i->extent.height;
        if(!i->arrayLayers || plane>SIZE_MAX/4/i->arrayLayers)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        uint64_t pixels=plane*i->arrayLayers;
        /* A colour readback reads a 32-bit-per-texel surface: the normalized
         * attachment this profile has always read back, and - only in the
         * build that serves it - the integer one the independentBlend oracle
         * uses. Both are tiled by the same 64KB_R_X equation. */
        if(!(i->format==VK_FORMAT_R8G8B8A8_UNORM ||
             ps5vk_color_target_integer_served(i->format)) ||
           i->samples!=VK_SAMPLE_COUNT_1_BIT || i->mipLevels!=1 ||
           i->arrayLayers!=1 || i->extent.depth!=1 ||
           (i->usage&required)!=required || !pixels || pixels>SIZE_MAX/4 ||
           r->bufferOffset || (r->bufferRowLength && r->bufferRowLength!=i->extent.width) ||
           (r->bufferImageHeight && r->bufferImageHeight!=i->extent.height) ||
           r->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
           r->imageSubresource.mipLevel || r->imageSubresource.baseArrayLayer ||
           r->imageSubresource.layerCount!=1 || r->imageOffset.x || r->imageOffset.y ||
           r->imageOffset.z || r->imageExtent.width!=i->extent.width ||
           r->imageExtent.height!=i->extent.height || r->imageExtent.depth!=1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        void *source,*destination;VkDeviceSize source_bytes,destination_bytes;
        const size_t tiled=ps5vk_color_64k_rx_surface_size(4,i->extent.width,i->extent.height);
        if(tiled==SIZE_MAX ||
           ps5vk_image_span(d,image,&source,&source_bytes)!=VK_SUCCESS ||
           ps5vk_buffer_span(d,copy->copy_destination,0,VK_WHOLE_SIZE,
               &destination,&destination_bytes)!=VK_SUCCESS ||
           source_bytes<tiled || destination_bytes<pixels*4)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        const VkDeviceSize stride=source_bytes/i->arrayLayers;
        if(source_bytes%i->arrayLayers || stride<tiled)return VK_ERROR_FEATURE_NOT_PRESENT;
        uintptr_t src=(uintptr_t)source,dst=(uintptr_t)destination;
        if(src<dst ? source_bytes>dst-src : pixels*4>src-dst)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        /* Two targets are two surfaces and two buffers: a repeat would read or
         * write one of them twice, which this shape must not express. */
        for(unsigned k=0;k<t;++k)
            if(out->target[k].buffer==copy->copy_destination)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        out->target[t].buffer=copy->copy_destination;
        out->target[t].layer_stride=stride;
        ++at;
    }
    /* The trailing scope names every buffer the copies published into. The
     * recorder appends one aggregate behind the named entries when a barrier
     * call names no image, so the tail is the named buffers followed by at most
     * that one aggregate, and it must name at least one buffer. */
    if(at>=count || ops[at].type!=PS5VK_BARRIER)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    unsigned named=0,aggregate=0;
    for(;at<count;++at) {
        if(ops[at].type!=PS5VK_BARRIER)return VK_ERROR_FEATURE_NOT_PRESENT;
        if(ops[at].buffer_barrier.buffer) {
            if(aggregate)return VK_ERROR_FEATURE_NOT_PRESENT;
            ++named;
        } else if(++aggregate>1u)return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(!named)return VK_ERROR_FEATURE_NOT_PRESENT;
    return VK_SUCCESS;
}

/* Caller must have observed the exact GPU completion serial and invalidated
 * source cache lines. This copies real GPU bytes, not a rendering oracle. */
static inline int ps5vk_readback_detile(VkImage image, size_t stride,
    void *destination,size_t destination_bytes,const void *source,size_t source_bytes)
{
    if(!image || !destination || !source || !image->info.arrayLayers || !stride ||
       !image->info.extent.width || !image->info.extent.height ||
       image->info.extent.width>SIZE_MAX/4/image->info.extent.height ||
       stride>source_bytes/image->info.arrayLayers)return -1;
    const size_t plane=(size_t)image->info.extent.width*image->info.extent.height*4;
    if(!plane || plane>destination_bytes/image->info.arrayLayers)return -1;
    /* A depth surface is addressed by the 64KB_Z_X equation, not the colour
     * RX one, and it is always a single layer in this profile. */
    if(ps5vk_depth_readback_image(image))
        return image->info.arrayLayers==1u &&
            !ps5vk_depth_64k_zx_detile(destination,plane,source,stride,
                image->info.extent.width,image->info.extent.height) ? 0 : -1;
    for(uint32_t layer=0;layer<image->info.arrayLayers;++layer)
        if(ps5vk_rgba8_64k_rx_detile((unsigned char *)destination+layer*plane,plane,
            (const unsigned char *)source+layer*stride,stride,
            image->info.extent.width,image->info.extent.height))return -1;
    return 0;
}

/* DXVK262-T10: the general colour readback postlude.
 *
 * The strict shapes above are the CTS modules' own sequences. A D3D11 runtime
 * such as the pinned DXVK 2.6.2 reads a render target back with a different,
 * equally valid one (measured on its first frame, dxvk_context.cpp:3989-4030
 * and dxvk_barrier.cpp:444-517): a global dependency from the attachment
 * writes, the handover of the image to TRANSFER_SRC_OPTIMAL, one or more
 * vkCmdCopyImageToBuffer2 regions into a sub-allocated staging buffer (a
 * nonzero bufferOffset and an explicit bufferRowLength/bufferImageHeight), and
 * one final dependency whose global member publishes the transfer writes to
 * the host and whose image member returns the image to its attachment layout.
 *
 * The postlude is therefore walked in order, and exactly three kinds of
 * operation are admitted:
 *
 *   PS5VK_BARRIER        a global or buffer dependency. It orders work but
 *                        moves no data; the one that follows the last copy and
 *                        covers TRANSFER_WRITE -> HOST_READ is the host
 *                        publication every readback requires.
 *   PS5VK_IMAGE_BARRIER  a readback colour image handed from its attachment
 *                        layout (or GENERAL) to TRANSFER_SRC_OPTIMAL, or back.
 *                        Only the layout moves; the tiled bytes are untouched.
 *   PS5VK_COPY_IMAGE_BUFFER  one region of a 32-bit colour image in
 *                        TRANSFER_SRC_OPTIMAL into a buffer, at mip 0, over any
 *                        layer range and rectangle inside the image, at any
 *                        texel-aligned bufferOffset, with any row length and
 *                        image height at least the copied extent.
 *
 * Every copy becomes one plan the completion path executes on the CPU after
 * the job's exact GPU serial, detiling only the region into the buffer bytes
 * the region addresses. Nothing else - a draw, a clear, an upload, a depth
 * aspect, a mip level above zero, a region that leaves the image or the
 * buffer, a destination that overlaps a source image, a copy with no host
 * publication after it - is admitted; the caller keeps refusing it. */
struct ps5vk_readback_regions {
    struct ps5vk_readback_plan target[PS5VK_MAX_READBACK_REGIONS];
    unsigned count;
};
_Static_assert((int)PS5VK_MAX_READBACK_REGIONS >= (int)PS5VK_MAX_COLOR_ATTACHMENTS,
    "a readback set must hold every colour target");

static inline int ps5vk_readback_region_image(VkImage image)
{
    return image && (ps5vk_colour_transfer_image(image) ||
        ps5vk_basic_colour_readback_image(image) || ps5vk_array_color_image(image)) &&
        (image->info.format == VK_FORMAT_R8G8B8A8_UNORM ||
         ps5vk_color_target_integer_served(image->info.format)) &&
        image->info.mipLevels == 1 && image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.extent.depth == 1 && image->info.arrayLayers &&
        (image->info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

static inline VkResult ps5vk_readback_regions_commands(VkDevice d,
    const struct ps5vk_operation *ops, unsigned count,
    struct ps5vk_layout_state *layouts, struct ps5vk_readback_regions *out,
    unsigned *failure_site)
{
#define REGIONS_REFUSE(site) do { if (failure_site) *failure_site = (site); \
    return VK_ERROR_FEATURE_NOT_PRESENT; } while (0)
    if (!d || !ops || !count || !layouts || !out) REGIONS_REFUSE(40);
    memset(out, 0, sizeof(*out));
    struct ps5vk_layout_state updated = *layouts;
    int published = 1;
    for (unsigned k = 0; k < count; ++k) {
        const struct ps5vk_operation *op = &ops[k];
        if (op->type == PS5VK_BARRIER) {
            if (op->buffer_barrier.buffer) {
                /* A buffer member must name a buffer a copy of this postlude
                 * writes; its access decides whether it publishes. */
                int named = 0;
                for (unsigned t = 0; t < out->count; ++t)
                    named |= out->target[t].buffer == op->buffer_barrier.buffer;
                if (!named) REGIONS_REFUSE(41);
            }
            if ((op->src_stage & VK_PIPELINE_STAGE_TRANSFER_BIT ||
                 op->src_stage & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) &&
                (op->src_access & VK_ACCESS_TRANSFER_WRITE_BIT) &&
                (op->dst_stage & VK_PIPELINE_STAGE_HOST_BIT) &&
                (op->dst_access & VK_ACCESS_HOST_READ_BIT))
                published = 1;
            continue;
        }
        if (op->type == PS5VK_IMAGE_BARRIER) {
            const VkImageMemoryBarrier *b = &op->image_barrier;
            const VkImageLayout home = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            const int handover = (b->oldLayout == home || b->oldLayout == VK_IMAGE_LAYOUT_GENERAL) &&
                b->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const int handback = b->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                (b->newLayout == home || b->newLayout == VK_IMAGE_LAYOUT_GENERAL);
            if (!ps5vk_readback_region_image(b->image) || b->image->device != d ||
                (!handover && !handback) ||
                ps5vk_layout_transition(&updated, b->image, b->oldLayout, b->newLayout) != VK_SUCCESS)
                REGIONS_REFUSE(42);
            continue;
        }
        if (op->type != PS5VK_COPY_IMAGE_BUFFER) REGIONS_REFUSE(43);
        VkImage image = op->copy_image;
        const VkBufferImageCopy *r = &op->copy_region;
        if (out->count == PS5VK_MAX_READBACK_REGIONS || !ps5vk_readback_region_image(image) ||
            image->device != d || op->copy_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
            !op->copy_destination ||
            ps5vk_layout_require(&updated, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) != VK_SUCCESS)
            REGIONS_REFUSE(44);
        const uint64_t bytes = ps5vk_readback_region_bytes(image, r);
        void *source, *destination;
        VkDeviceSize source_bytes, destination_bytes;
        const size_t tiled = ps5vk_color_64k_rx_surface_size(4, image->info.extent.width,
            image->info.extent.height);
        if (!bytes || tiled == SIZE_MAX ||
            ps5vk_image_span(d, image, &source, &source_bytes) != VK_SUCCESS ||
            ps5vk_buffer_span(d, op->copy_destination, 0, VK_WHOLE_SIZE, &destination,
                &destination_bytes) != VK_SUCCESS ||
            r->bufferOffset > destination_bytes || bytes > destination_bytes - r->bufferOffset ||
            source_bytes % image->info.arrayLayers ||
            source_bytes / image->info.arrayLayers < tiled ||
            (image->info.arrayLayers > 1 && (source_bytes / image->info.arrayLayers) % 131072u))
            REGIONS_REFUSE(45);
        const uintptr_t src = (uintptr_t)source;
        const uintptr_t dst = (uintptr_t)destination + (uintptr_t)r->bufferOffset;
        if (src < dst ? source_bytes > dst - src : bytes > src - dst) REGIONS_REFUSE(46);
        out->target[out->count++] = (struct ps5vk_readback_plan){image, op->copy_destination,
            source_bytes / image->info.arrayLayers, 0, VK_TRUE, *r};
        published = 0;
    }
    if (!out->count || !published) REGIONS_REFUSE(47);
    *layouts = updated;
    return VK_SUCCESS;
#undef REGIONS_REFUSE
}

/* Detile exactly one admitted region: every texel of the rectangle, of every
 * layer, from the 64KB_R_X surface into the buffer at the region's own
 * offset, row length and image height. Texels outside the region and buffer
 * bytes outside it are never written. */
static inline int ps5vk_readback_region_detile(VkImage image, size_t stride,
    const VkBufferImageCopy *r, void *destination, size_t destination_bytes,
    const void *source, size_t source_bytes)
{
    if (!image || !r || !destination || !source || !stride) return -1;
    const uint64_t bytes = ps5vk_readback_region_bytes(image, r);
    if (!bytes || r->bufferOffset > destination_bytes ||
        bytes > destination_bytes - r->bufferOffset ||
        (uint64_t)stride * (r->imageSubresource.baseArrayLayer +
            r->imageSubresource.layerCount) > source_bytes)
        return -1;
    const size_t row = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width;
    const size_t height = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
    const uint32_t width = image->info.extent.width;
    for (uint32_t l = 0; l < r->imageSubresource.layerCount; ++l) {
        const unsigned char *layer = (const unsigned char *)source +
            (size_t)(r->imageSubresource.baseArrayLayer + l) * stride;
        unsigned char *out = (unsigned char *)destination + r->bufferOffset +
            (size_t)l * row * height * 4u;
        for (uint32_t y = 0; y < r->imageExtent.height; ++y)
            for (uint32_t x = 0; x < r->imageExtent.width; ++x) {
                const size_t at = ps5vk_rgba8_64k_rx_offset((uint32_t)r->imageOffset.x + x,
                    (uint32_t)r->imageOffset.y + y, width);
                if (at == SIZE_MAX || at > stride - 4u) return -1;
                memcpy(out + ((size_t)y * row + x) * 4u, layer + at, 4u);
            }
    }
    return 0;
}

#endif

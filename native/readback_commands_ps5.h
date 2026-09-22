#ifndef PS5VK_READBACK_COMMANDS_PS5_H
#define PS5VK_READBACK_COMMANDS_PS5_H
#include "vk_command.h"
#include "image_layout_state.h"
#include "color_detile.h"
#include "depth_detile.h"
#include "vk_image.h"
#include "color_attachment_contract.h"
#include "vk_image_transfer.h"

struct ps5vk_readback_plan { VkImage image; VkBuffer buffer; VkDeviceSize layer_stride; };

struct ps5vk_readback_partition {
    unsigned prefix_count;
    unsigned readback_first;
    unsigned readback_count;
};

/* A render-pass postlude may order unrelated shader writes before the fixed
 * image readback sequence.  Locate that sequence by its single image-to-buffer
 * copy, whose immediately preceding operation must be its image barrier.  The
 * strict readback validator below still owns the complete four-operation
 * suffix; this function only partitions the immutable command record and
 * refuses ambiguous or trailing shapes. */
static inline VkResult ps5vk_readback_partition(
    const struct ps5vk_operation *ops,unsigned count,
    struct ps5vk_readback_partition *out)
{
    if(!ops || !count || !out)return VK_ERROR_FEATURE_NOT_PRESENT;
    unsigned copy=count,copies=0;
    for(unsigned i=0;i<count;++i)
        if(ops[i].type==PS5VK_COPY_IMAGE_BUFFER){copy=i;++copies;}
    if(copies!=1 || !copy || count-copy!=3)return VK_ERROR_FEATURE_NOT_PRESENT;
    const unsigned first=copy-1;
    if(ops[first].type!=PS5VK_IMAGE_BARRIER)return VK_ERROR_FEATURE_NOT_PRESENT;
    *out=(struct ps5vk_readback_partition){first,first,4};
    return VK_SUCCESS;
}

/* The same bounded full-color readback may follow a render pass or be a
 * separate submission. This only validates and stages the layout: the caller
 * must flush CB/DB caches and observe its exact GPU serial before detiling or
 * publishing host bytes. A plan is not evidence of a completed transfer. */
static inline VkResult ps5vk_readback_commands(VkDevice d,
    const struct ps5vk_operation *ops,unsigned count,VkImage color,
    struct ps5vk_layout_state *layouts,struct ps5vk_readback_plan *out)
{
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
    if(!d || !ops || !layouts || !out || (count!=4 && count!=3))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const int staged=count==4;
    if(staged ? (ops[0].type!=PS5VK_IMAGE_BARRIER || ops[1].type!=PS5VK_COPY_IMAGE_BUFFER ||
                 ops[2].type!=PS5VK_BARRIER || ops[3].type!=PS5VK_BARRIER)
              : (ops[0].type!=PS5VK_COPY_IMAGE_BUFFER || ops[1].type!=PS5VK_BARRIER ||
                 ops[2].type!=PS5VK_BARRIER))
        return VK_ERROR_FEATURE_NOT_PRESENT;
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
        return VK_ERROR_FEATURE_NOT_PRESENT;
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
          !(ps5vk_array_color_image(image) && b->oldLayout==VK_IMAGE_LAYOUT_GENERAL)) ||
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
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkBufferImageCopy *r=&copy->copy_region;
    const VkImageCreateInfo *i=&image->info;
    const VkImageUsageFlags required=(depth_source?
        (VkImageUsageFlags)VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
        (VkImageUsageFlags)VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    uint64_t plane=(uint64_t)i->extent.width*i->extent.height;
    if(!i->arrayLayers || plane>SIZE_MAX/4/i->arrayLayers)return VK_ERROR_FEATURE_NOT_PRESENT;
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
        return VK_ERROR_FEATURE_NOT_PRESENT;
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
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Native array allocations consist of equally-sized, 128 KiB-aligned
     * layer footprints. Never infer a tight width*height stride for tiled data. */
    VkDeviceSize stride=source_bytes/i->arrayLayers;
    if(source_bytes%i->arrayLayers || stride<tiled ||
       (i->arrayLayers>1 && stride%131072u))return VK_ERROR_FEATURE_NOT_PRESENT;
    uintptr_t src=(uintptr_t)source,dst=(uintptr_t)destination;
    if(src<dst ? source_bytes>dst-src : pixels*4>src-dst)return VK_ERROR_FEATURE_NOT_PRESENT;
    /* The unstaged shape carries no transition of its own: the surface is
     * already where the copy needs it, so there is nothing to stage. */
    if(staged) {
        VkResult rc=ps5vk_layout_transition(layouts,image,b->oldLayout,b->newLayout);
        if(rc!=VK_SUCCESS)return rc;
    }
    *out=(struct ps5vk_readback_plan){image,copy->copy_destination,stride};
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
#endif

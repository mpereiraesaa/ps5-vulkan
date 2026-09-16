#ifndef PS5VK_UPLOAD_COMMANDS_PS5_H
#define PS5VK_UPLOAD_COMMANDS_PS5_H
#include "vk_command.h"
#include "vk_image_transfer.h"
#include "image_layout_state.h"
#include "texture_copy.h"
#include "texture_dma.h"
#include "graphics_sync.h"
#include "color_barrier.h"

/* Shared by a render prelude and an independent transfer submission. Prepare
 * only emits commands and records tentative layouts: it never copies pixels
 * or commits resource state before the GPU completion label. */
static inline VkResult ps5vk_upload_commands(VkDevice d,
    const struct ps5vk_operation *ops, unsigned count, VkImage color,
    struct ps5vk_layout_state *layouts, uint32_t **cursor, uint32_t *end,
    void (*flush)(const void *,size_t))
{
    for(unsigned i=0;i<count;++i) {
        const struct ps5vk_operation *op=&ops[i];
        size_t n=0;
        if(op->type==PS5VK_BARRIER) {
            /* Existing vertex-host prelude or host-written upload staging.
             * Range validity/lifetime is also checked by the frontend. */
            const int vertex=!op->buffer_barrier.buffer &&
                op->src_stage==VK_PIPELINE_STAGE_HOST_BIT &&
                op->dst_stage==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
                op->src_access==VK_ACCESS_HOST_WRITE_BIT &&
                op->dst_access==VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            const int upload=op->src_stage==VK_PIPELINE_STAGE_HOST_BIT &&
                op->dst_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                op->src_access==VK_ACCESS_HOST_WRITE_BIT &&
                op->dst_access==VK_ACCESS_TRANSFER_READ_BIT;
            /* The pinned upstream draw case orders the transfer write that
             * initialised and cleared its colour target against the
             * colour-attachment stages (vktDrawBaseClass.cpp:207-211). The
             * barrier names no resource of its own, so it is the same acquire
             * the prelude already emits, bounded to exactly that pair of stages
             * and accesses. */
            const int color_prelude=!op->buffer_barrier.buffer &&
                op->src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                op->dst_stage==VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
                op->src_access==VK_ACCESS_TRANSFER_WRITE_BIT &&
                op->dst_access==(VkAccessFlags)(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|
                                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            if(!vertex && !upload && !color_prelude)return VK_ERROR_FEATURE_NOT_PRESENT;
            if(op->buffer_barrier.buffer) {
                void *address;VkDeviceSize bytes;
                VkResult rc=ps5vk_buffer_span(d,op->buffer_barrier.buffer,
                    op->buffer_barrier.offset,op->buffer_barrier.size,&address,&bytes);
                if(rc!=VK_SUCCESS)return rc;
                flush(address,(size_t)bytes);
            }
            n=ps5vk_graphics_acquire(*cursor,(size_t)(end-*cursor));
        } else if(op->type==PS5VK_IMAGE_BARRIER) {
            const VkImageMemoryBarrier *b=&op->image_barrier;
            if(!((b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
                 b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                 !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT) ||
                (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                 b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && b->dstAccessMask==VK_ACCESS_SHADER_READ_BIT) ||
                /* The pinned draw case's first transition: the RGBA8 colour
                 * attachment that also declares a transfer destination goes
                 * UNDEFINED -> GENERAL for the transfer write that follows, with
                 * the same stage and access pair the recorder accepts. This is
                 * the prelude that must agree with that record. */
                (ps5vk_colour_transfer_image(b->image) &&
                 b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
                 b->newLayout==VK_IMAGE_LAYOUT_GENERAL &&
                 !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
                 op->src_stage==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT &&
                 op->dst_stage==VK_PIPELINE_STAGE_TRANSFER_BIT) ||
                /* The cleared depth target becoming a depth attachment. This is
                 * the transition that makes an explicit clear controllable by a
                 * later depth test, so it is bounded to exactly that: a D32
                 * clear target, the transfer write it just received, and the
                 * depth/stencil attachment access the fragment tests perform.
                 * The destination stage mask may name either fragment-test
                 * stage or both: a pipeline may test depth at either, and the
                 * emitted ordering is the same conservative acquire, so
                 * demanding both would refuse a narrower valid barrier. */
                (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                 b->newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
                 ps5vk_depth_clear_image(b->image) &&
                 b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
                 b->dstAccessMask==(VkAccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) &&
                 op->src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                 (op->dst_stage & (VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)) &&
                 !(op->dst_stage & ~(VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT))) ||
                ((!color || b->image==color) && ps5vk_color_discard_barrier(b)) ||
                ps5vk_array_color_barrier(b)))
                return VK_ERROR_FEATURE_NOT_PRESENT;
            VkResult rc=ps5vk_layout_transition(layouts,b->image,b->oldLayout,b->newLayout);
            if(rc!=VK_SUCCESS)return rc;
            n=ps5vk_graphics_acquire(*cursor,(size_t)(end-*cursor));
        } else if(op->type==PS5VK_COPY_BUFFER_IMAGE) {
            if(op->copy_layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)return VK_ERROR_FEATURE_NOT_PRESENT;
            void *source,*destination;VkDeviceSize source_bytes,destination_bytes;
            struct ps5vk_texture_copy copy;
            VkResult rc=ps5vk_layout_require(layouts,op->copy_image,op->copy_layout);
            if(rc!=VK_SUCCESS)return rc;
            rc=ps5vk_buffer_span(d,op->copy_source,0,VK_WHOLE_SIZE,&source,&source_bytes);
            if(rc!=VK_SUCCESS)return rc;
            rc=ps5vk_image_span(d,op->copy_image,&destination,&destination_bytes);
            if(rc!=VK_SUCCESS)return rc;
            rc=ps5vk_texture_copy_plan_for_image(op->copy_image,source_bytes,destination_bytes,&op->copy_region,&copy);
            if(rc!=VK_SUCCESS)return rc;
            flush(source,(size_t)source_bytes);
            n=ps5vk_texture_dma(*cursor,(size_t)(end-*cursor),(uintptr_t)source,(uintptr_t)destination,&copy);
        } else if(op->type==PS5VK_CLEAR_DEPTH_STENCIL_IMAGE || op->type==PS5VK_CLEAR_COLOR_IMAGE) {
            /* The same uniform-DWORD fill the render pass emits for its depth
             * load-op clear. Every texel of the one-sample D32 surface takes
             * the identical word, so the whole allocation is filled and the
             * 64KB_Z_X pixel equations are not needed or claimed. */
            if(op->type==PS5VK_CLEAR_COLOR_IMAGE ? !ps5vk_array_color_clear(op) :
                !ps5vk_depth_clear_image(op->image_destination))return VK_ERROR_FEATURE_NOT_PRESENT;
            void *destination;VkDeviceSize destination_bytes;
            VkResult rc=ps5vk_layout_require(layouts,op->image_destination,
                op->image_destination_layout);
            if(rc!=VK_SUCCESS)return rc;
            rc=ps5vk_image_span(d,op->image_destination,&destination,&destination_bytes);
            if(rc!=VK_SUCCESS)return rc;
            /* Drop any stale host lines over the target before the GPU writes
             * it, exactly as the render-pass depth clear does. */
            flush(destination,(size_t)destination_bytes);
            n=ps5vk_dma_fill(*cursor,(size_t)(end-*cursor),(uintptr_t)destination,
                destination_bytes,op->clear_word);
        } else return VK_ERROR_FEATURE_NOT_PRESENT;
        if(!n)return VK_ERROR_UNKNOWN;
        *cursor+=n;
    }
    return VK_SUCCESS;
}
#endif

#ifndef PS5VK_UPLOAD_COMMANDS_PS5_H
#define PS5VK_UPLOAD_COMMANDS_PS5_H
#include "vk_command.h"
#include "vk_image_transfer.h"
#include "image_layout_state.h"
#include "texture_copy.h"
#include "texture_dma.h"
#include "graphics_sync.h"
#include "color_barrier.h"
#include "vk_image.h"
#include "sample_rate_contract.h"

/* Shared by a render prelude and an independent transfer submission. Prepare
 * only emits commands and records tentative layouts: it never copies pixels
 * or commits resource state before the GPU completion label. */
/* DXVK262-T10: the stage scopes a D3D11 runtime names around its render
 * targets: some of the transfer, colour-output, fragment and whole-pipeline
 * stages, nothing else, and never empty. */
static inline int ps5vk_dxvk_attachment_stages(VkPipelineStageFlags stages)
{
    const VkPipelineStageFlags allowed = VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
        VK_PIPELINE_STAGE_HOST_BIT;
    return stages && !(stages & ~allowed);
}
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
            /* A fragment shader may write an SSBO and expose it to the host
             * after this submission completes.  The command-buffer frontend
             * has already validated the exact buffer range and stage/access
             * scopes.  Flush the host mapping before submit so stale CPU
             * cache lines cannot overwrite the shader result; the job's
             * final RELEASE_MEM performs the GPU writeback before completion
             * is reported to the host.  Keep this deliberately narrower than
             * a generic shader barrier: it is the measured CTS contract. */
            const int fragment_host=op->buffer_barrier.buffer &&
                op->src_stage==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT &&
                op->dst_stage==VK_PIPELINE_STAGE_HOST_BIT &&
                op->src_access==VK_ACCESS_SHADER_WRITE_BIT &&
                op->dst_access==VK_ACCESS_HOST_READ_BIT;
            /* vkCmdPipelineBarrier records one aggregate dependency after
             * every buffer-only call.  With no VkMemoryBarrier in that call,
             * the aggregate intentionally carries zero access masks; the
             * preceding per-buffer operation retains the actual scope and
             * range.  Accept this otherwise resource-less operation only as
             * the second half of the exact fragment SSBO -> host pair above.
             * This keeps an isolated or differently scoped zero-access
             * aggregate fail-closed. */
            const struct ps5vk_operation *previous=i?&ops[i-1]:NULL;
            const int fragment_host_aggregate=!op->buffer_barrier.buffer &&
                op->src_stage==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT &&
                op->dst_stage==VK_PIPELINE_STAGE_HOST_BIT &&
                !op->src_access && !op->dst_access && previous &&
                previous->type==PS5VK_BARRIER &&
                previous->buffer_barrier.buffer &&
                previous->src_stage==op->src_stage &&
                previous->dst_stage==op->dst_stage &&
                previous->src_access==VK_ACCESS_SHADER_WRITE_BIT &&
                previous->dst_access==VK_ACCESS_HOST_READ_BIT;
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
            if(!vertex && !upload && !color_prelude && !fragment_host &&
               !fragment_host_aggregate)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if(op->buffer_barrier.buffer) {
                void *address;VkDeviceSize bytes;
                VkResult rc=ps5vk_buffer_span(d,op->buffer_barrier.buffer,
                    op->buffer_barrier.offset,op->buffer_barrier.size,&address,&bytes);
                if(rc!=VK_SUCCESS)return rc;
                flush(address,(size_t)bytes);
            }
            n=ps5vk_graphics_acquire(*cursor,(size_t)(end-*cursor));
        } else if(op->type==PS5VK_IMAGE_BARRIER &&
                  ps5vk_depth_stencil_attachment_image(op->image_barrier.image)) {
            /* The combined depth/stencil attachment moves only the aspects the
             * barrier names. The recorder already refused the per-aspect forms
             * a device without separateDepthStencilLayouts may not use, so the
             * executor re-checks the shape and lets the per-aspect transaction
             * decide whether each old layout matches. The acquire is the same
             * conservative cache operation every other image barrier emits. */
            const VkImageMemoryBarrier *b=&op->image_barrier;
            if(!ps5vk_depth_stencil_barrier(b,VK_TRUE,VK_TRUE))return VK_ERROR_FEATURE_NOT_PRESENT;
            VkResult rc=ps5vk_layout_transition_aspects(layouts,b->image,
                b->subresourceRange.aspectMask,b->oldLayout,b->newLayout);
            if(rc!=VK_SUCCESS)return rc;
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
                (ps5vk_d16_attachment_image(b->image) &&
                 b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
                 b->newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
                 !b->srcAccessMask &&
                 b->dstAccessMask==VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT &&
                 (op->src_stage==VK_PIPELINE_STAGE_HOST_BIT ||
                  op->src_stage==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) &&
                 op->dst_stage==(VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)) ||
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
                 /* The write is the point of the transition; the read is the
                  * caller's to declare. The pinned upstream depth clamp module
                  * names the write alone, an earlier measured case named both. */
                 (b->dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) &&
                 !(b->dstAccessMask & ~(VkAccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)) &&
                 op->src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                 /* The depth test runs in one or both fragment-test stages.
                  * ALL_GRAPHICS names the whole graphics pipeline, so it
                  * contains both, and the pinned upstream depth clamp module
                  * hands its cleared target to the draw with exactly that. */
                 (op->dst_stage & (VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
                                                         VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)) &&
                 !(op->dst_stage & ~(VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
                                                           VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT))) ||
                /* The rendered depth surface handed to its readback. A
                 * DEPTH-ONLY pass ends with exactly this transition recorded
                 * after the render pass, and the recorder already accepts it
                 * for a depth image that declares the transfer-source role
                 * (ps5vk_depth_readback_image); the executor has to agree with
                 * that record or the submission refuses what recording let
                 * through. The depth test writes in the fragment-test stages,
                 * which ALL_GRAPHICS also contains. */
                (ps5vk_depth_readback_image(b->image) &&
                 b->oldLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
                 b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                 b->srcAccessMask==VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT &&
                 b->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT &&
                 (op->src_stage & (VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
                                                         VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)) &&
                 !(op->src_stage & ~(VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                                                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|
                                                           VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)) &&
                 op->dst_stage==VK_PIPELINE_STAGE_TRANSFER_BIT) ||
                /* The same clear-through-transfer pair the recorder accepts for
                 * a colour attachment that also declares a transfer
                 * destination. The executor has to agree with that record or
                 * the submission refuses what recording let through. */
                (ps5vk_colour_transfer_image(b->image) &&
                 ((b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
                   b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
                   op->dst_stage==VK_PIPELINE_STAGE_TRANSFER_BIT) ||
                  (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   b->newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                   b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
                   b->dstAccessMask==VK_ACCESS_SHADER_WRITE_BIT &&
                   op->src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT) ||
                  /* The pinned compressed-texture renderer clears its
                   * RGBA8 readback target, then hands it to the colour
                   * attachment stage with the ordinary colour-write access
                   * and ALL_COMMANDS destination stage
                   * (vktTextureTestUtil.cpp:1167-1185). */
                  (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   b->newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                   b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
                   b->dstAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
                   op->src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                   op->dst_stage==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) ||
                  /* The pinned render-pass module's own initialization pair:
                   * the acquire that discards each attachment into its
                   * transfer destination and the handover that gives the
                   * cleared attachment to its attachment layout, both recorded
                   * from the transfer stage to the whole engine, host
                   * included. The recorder accepts exactly these two shapes
                   * (src/color_barrier.h), so the executor has to agree with
                   * them or a submission refuses what recording let through. */
                  ((ps5vk_attachment_initialization_acquire_barrier(b) ||
                    ps5vk_attachment_initialization_handover_barrier(b)) &&
                   op->src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                   op->dst_stage==(VkPipelineStageFlags)(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|
                                                         VK_PIPELINE_STAGE_HOST_BIT)) ||
                  /* DXVK262-T10: a D3D11 runtime's cleared render target is
                   * handed to its attachment layout from the transfer stage
                   * to the stages that next use it - DXVK 2.6.2 names
                   * COLOR_ATTACHMENT_OUTPUT|TRANSFER (0x1400) with access
                   * 0x1980, in a submission of its own. The executor runs
                   * jobs serially and flushes the transfer write before the
                   * next job, so only the layout moves. */
                  (ps5vk_attachment_initialization_handover_barrier(b) &&
                   op->src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                   ps5vk_dxvk_attachment_stages(op->dst_stage)))) ||
                /* DXVK262-T10: the readback hand-over and hand-back with no
                 * source access, ordered by the global dependency DXVK records
                 * before them (src/color_barrier.h). */
                (ps5vk_colour_readback_dependency_barrier(b) &&
                 ps5vk_dxvk_attachment_stages(op->src_stage) &&
                 ps5vk_dxvk_attachment_stages(op->dst_stage)) ||
                /* The rendered colour surface handed to its readback. When the
                 * copy shares the submission this is part of the four-operation
                 * readback shape; when the readback is submitted separately the
                 * transition is all this range carries, and the executor has to
                 * agree with the record either way. */
                (ps5vk_colour_readback_image(b->image) &&
                 b->oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                 b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                 b->srcAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
                 b->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT &&
                 op->src_stage==VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
                 op->dst_stage==VK_PIPELINE_STAGE_TRANSFER_BIT) ||
                ps5vk_precise_query_colour_barrier(b,op->src_stage,op->dst_stage) ||
                /* The pinned texture renderer restores its colour target
                 * after copyImageToBuffer. Keep this explicit handback in the
                 * same graphics serial as the readback and preserve the final
                 * COLOR_ATTACHMENT_OPTIMAL layout for the following pass. */
                (ps5vk_colour_readback_image(b->image) &&
                 b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                 b->newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                 b->srcAccessMask==VK_ACCESS_TRANSFER_READ_BIT &&
                 b->dstAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
                 op->src_stage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                 op->dst_stage==VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) ||
                ((!color || b->image==color) && ps5vk_color_discard_barrier(b)) ||
                ((!color || b->image==color) && ps5vk_color_readback_reuse_barrier(b)) ||
                /* The pinned multisample leaves' own first-use transition: the
                 * multisampled colour image and the single-sample attachments
                 * the oracle reads back each go from UNDEFINED to
                 * COLOR_ATTACHMENT_OPTIMAL for the colour-attachment write, from
                 * TOP_OF_PIPE to COLOR_ATTACHMENT_OUTPUT. That is the ordinary
                 * "first use as a render target" barrier, and the executor
                 * performs exactly this transition itself in the pass prelude;
                 * the recorder already accepts it for these roles, so refusing
                 * it here refused a submission the front end had let through
                 * (measured: min_sample_shading.min_0_0.samples_2.primitive_triangle
                 * -> vkQueueSubmit VK_ERROR_FEATURE_NOT_PRESENT, prelude site 701). */
                (b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
                 b->newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                 !b->srcAccessMask &&
                 b->dstAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
                 op->src_stage==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT &&
                 op->dst_stage==VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
                 (ps5vk_colour_readback_image(b->image) ||
                  (b->image->info.samples!=VK_SAMPLE_COUNT_1_BIT &&
                   ps5vk_multisampled_color_usage(b->image->info.usage)))) ||
                ps5vk_array_color_barrier(b) ||
                ps5vk_bgra8_transfer_barrier(b) ||
                (ps5vk_tiled_cube_sampled_image(b->image) &&
                 b->oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                 b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 b->srcAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
                 b->dstAccessMask==VK_ACCESS_SHADER_READ_BIT &&
                 op->src_stage==VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
                 op->dst_stage==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)))
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

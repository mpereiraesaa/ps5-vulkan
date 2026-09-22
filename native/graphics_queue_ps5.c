#include "vk_queue.h"
#include "vk_indirect.h"
#include "draw_prepare_ps5.h"
#include "runtime_resource_use.h"
#include "graphics_descriptor_profile.h"
#include "readback_content.h"
#include "input_attachment_gate.h"
#include "command_arena_ps5.h"
#include "draw_batch_ps5.h"
#include "vertex_fetch.h"
#include "graphics_sync.h"
#include "image_layout_state.h"
#include "texture_copy.h"
#include "texture_dma.h"
#include "upload_commands_ps5.h"
#include "readback_commands_ps5.h"
#include "color_rect_clear.h"
#include "color_clear.h"
#include "color_detile.h"
#include "attachment_ops.h"
#include "ps5_platform.h"
#include "ps5_agc_driver.h"
#include "submit_suspend_ps5.h"
#include "ps5log.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include "tess_ring_lease.h"
extern int32_t sceAgcDriverGetTFRing(uint64_t *,uint32_t *);
extern int32_t sceAgcDriverSetTFRing(uint64_t,uint32_t);
extern int32_t sceAgcDriverGetHsOffchipParam(uint16_t *,uint16_t *);
static int ring_get(void *unused,uint64_t *address,uint32_t *size)
{ (void)unused;return sceAgcDriverGetTFRing(address,size); }
static int ring_set(void *unused,uint64_t address,uint32_t size)
{ (void)unused;return sceAgcDriverSetTFRing(address,size); }
#include "tess_offchip_lease.h"
extern int32_t sceAgcDriverSetHsOffchipParam(uint16_t,uint16_t);
static int offchip_get(void *unused,uint16_t *a,uint16_t *b)
{ (void)unused;return sceAgcDriverGetHsOffchipParam(a,b); }
static int offchip_set(void *unused,uint16_t a,uint16_t b)
{ (void)unused;return sceAgcDriverSetHsOffchipParam(a,b); }
/* One process-global lease, not one per command buffer.
 * Tess pipelines share per-device storage. Reject conflicting addresses rather
 * than silently rebinding an inconsistent job. No feature promotion here. */
static void *ring_owner;
/* Same FW ABI as the independently validated Xash3D native renderer. */
extern uint32_t *sceAgcDcbDrawIndex(void *,uint32_t,const void *,uint64_t);
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
/* GFX10 occlusion-counter probe. The begin and end event writes are built by
 * ps5vk_graphics_occlusion_event (src/graphics_sync.c, host-tested there);
 * begin addresses the slot base and end base+8. Each enabled render backend
 * writes a 64-bit start at 16*i and a 64-bit end at 16*i+8 and sets bit 63 of
 * each word when the dump lands, so the set of written pairs IS the enabled
 * render-backend mask. The slot is deliberately sized for a generous upper
 * bound: pairs no backend owns stay zero and are reported as unavailable
 * instead of being read as fabricated results. */
enum { PS5VK_OCCLUSION_PROBE_PAIRS = 64,
       PS5VK_OCCLUSION_PROBE_BYTES = PS5VK_OCCLUSION_PROBE_PAIRS * 16 };
#endif
struct graphics_job {
    struct ps5vk_tess_ring_lease ring;
    uint64_t ring_address;
    uint32_t offchip_bytes;
    struct ps5vk_hs_lease offchip;
#if defined(PS5VK_TESS_HULL_TRACE) && PS5VK_TESS_HULL_TRACE
    uint32_t *hull_trace;
#endif
    /* The ordered arena chain that holds this submission (draw_batch_ps5.h):
     * one arena for the ordinary pass, more when a multi-draw expansion or a
     * long pass outgrows one. `launched` counts the arenas already handed to
     * the GPU; poll() launches the next only after the previous label retired. */
    struct ps5vk_draw_batch_chain chain;
    struct ps5vk_command_arena slot;
    struct ps5vk_prepared_draw draws[PS5VK_MAX_OPERATIONS];
    unsigned count, words, attempted, complete, slot_active, launched;
    uint64_t serial, start;
    VkImage color;
    VkImage readback_image;
    VkBuffer readback_buffer;
    VkDeviceSize readback_stride;
    struct ps5vk_layout_state layouts;
};
static uint64_t now(void *unused)
{ (void)unused; struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t))return 0; return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec; }
static void pause_wait(void *unused,uint64_t remaining) { (void)unused; (void)remaining; usleep(1000); }
static void cache(const void *p,size_t n)
{
    uintptr_t end=(uintptr_t)p+n;
    for(uintptr_t a=(uintptr_t)p&~(uintptr_t)63;a<end;a+=64) __asm__ volatile("clflush (%0)"::"r"(a):"memory");
    __asm__ volatile("mfence":::"memory");
}
static void retain(const char *why)
{ ps5log_printf(PS5LOG_ERR,"PS5VK_GRAPHICS_RETAIN reason=%s",why); ps5log_close("graphics-retained"); for(;;)sleep(1); }
static void release(VkDevice d,void *opaque)
{
    (void)d; struct graphics_job *j=opaque;
    if(j->attempted && !j->complete)retain("inflight-release");
    if(j->ring.state!=PS5VK_TF_IDLE)retain("ring-release-unresolved");
    if(j->offchip.state!=PS5VK_HS_IDLE)retain("offchip-release-unresolved");
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    if(j->slot_active && ps5vk_command_arena_release(&j->slot)!=VK_SUCCESS)
        retain("occlusion-slot-release");
#endif
    if(ps5vk_draw_batch_release(&j->chain)!=VK_SUCCESS)retain("command-release");
    for(unsigned i=0;i<j->count;++i)ps5vk_native_release_draw(&j->draws[i]);
    free(j);
}
/* A draw that rasterizes nothing: Vulkan gives a zero vertex, index or
 * instance count no side effect, and the emitters write no packet for it. */
static int draw_has_work(const struct ps5vk_operation *op)
{
    return (op->type==PS5VK_DRAW_INDEXED?op->index_count:op->vertex_count) && op->instance_count;
}
/* Uploads, layout transitions and readback need not share a submission with a
 * draw. Reuse the render prelude/postlude and exact GPU completion protocol. */
static VkResult prepare_transfer(VkDevice d,const struct ps5vk_submission *s,
    VkCommandBuffer cb,unsigned first,unsigned count,void **out)
{
    if(!count)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct graphics_job *j=calloc(1,sizeof(*j));
    if(!j)return VK_ERROR_OUT_OF_HOST_MEMORY;
    j->serial=s->serial;
    /* The chain opens its first arena with the acquire already emitted. */
    VkResult rc=ps5vk_draw_batch_open(&j->chain,j->serial);
    if(rc==VK_ERROR_DEVICE_LOST)retain("upload-command-create");
    if(rc!=VK_SUCCESS)goto fail;
    uint32_t *cursor=j->chain.cursor,*end=j->chain.end;
    unsigned readback=0;
    for(unsigned i=0;i<count;++i)
        readback|=cb->operations[first+i].type==PS5VK_COPY_IMAGE_BUFFER;
    if(readback) {
        struct ps5vk_readback_plan plan={0};
        rc=ps5vk_readback_commands(d,cb->operations+first,count,NULL,&j->layouts,&plan);
        if(rc==VK_SUCCESS) {
            j->color=j->readback_image=plan.image;j->readback_buffer=plan.buffer;
            j->readback_stride=plan.layer_stride;
        }
    } else rc=ps5vk_upload_commands(d,cb->operations+first,count,NULL,&j->layouts,&cursor,end,cache);
    if(rc!=VK_SUCCESS)goto fail;
    j->chain.cursor=cursor;
    rc=ps5vk_draw_batch_close(&j->chain);
    if(rc!=VK_SUCCESS)goto fail;
    j->words=j->chain.words[0];*out=j;
    ps5log_printf(PS5LOG_MARK,"PS5VK_UPLOAD_PREPARED serial=%llu operations=%u words=%u readback=%u",
        (unsigned long long)j->serial,count,j->words,readback);
    return VK_SUCCESS;
fail:
    ps5log_printf(PS5LOG_ERR,"PS5VK_UPLOAD_PREPARE_FAILED serial=%llu rc=%d",
        (unsigned long long)j->serial,rc);
    release(d,j);return rc;
}
static VkResult prepare(VkDevice d,const struct ps5vk_submission *s,void **out)
{
    const char *phase="shape";
    /* Which refusal inside the draw phase fired. Every one of them returns the
     * same error code from a different line, so a failure reports "phase=draw"
     * and nothing else - which cost a window per gate while opening the
     * descriptor profile for a diagnostic. Numbering them makes one run name
     * the line. Zero means the failure was not one of the numbered sites. */
    unsigned draw_site=0;
    *out=NULL;
    /* One color pass, optional D32 and texture-upload prelude.  LOAD preserves
     * an attachment only when its tracked initial layout matches.  CLEAR is
     * bounded to the full render area until a rectangular clear path exists. */
    /* Buffer 0 is always the recording that owns the scope. A segment with
     * more buffers is a render pass whose work is NAMED by secondaries: they
     * follow in recorded order, each with its own range, and this is the only
     * shape that admits more than one. */
    if(!s->count || s->count>PS5VK_MAX_SUBMITTED_BUFFERS || !s->serial)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkCommandBuffer cb=s->buffers[0];
    uint32_t range_first=ps5vk_submission_first_operation(s,0);
    uint32_t range_count=ps5vk_submission_operation_count(s,0);
    if(range_first>cb->operation_count || range_count>cb->operation_count-range_first)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t range_end=range_first+range_count;
    unsigned first=range_first;
    while(first<range_end && cb->operations[first].type!=PS5VK_BEGIN_RENDER_PASS) {
        unsigned type=cb->operations[first].type;
        if(type!=PS5VK_BARRIER && type!=PS5VK_IMAGE_BARRIER &&
           type!=PS5VK_COPY_BUFFER_IMAGE && type!=PS5VK_COPY_IMAGE_BUFFER &&
           type!=PS5VK_CLEAR_DEPTH_STENCIL_IMAGE && type!=PS5VK_CLEAR_COLOR_IMAGE)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        ++first;
    }
    /* No render pass in this range: a transfer-only segment names one buffer. */
    if(first==range_end) {
        if(s->count!=1)return VK_ERROR_FEATURE_NOT_PRESENT;
        return prepare_transfer(d,s,cb,range_first,range_count,out);
    }
    unsigned last=first+1;
    while(last<range_end && cb->operations[last].type!=PS5VK_END_RENDER_PASS)++last;
    /* last<first+2 is a pass with no work between begin and end. Recording
     * already refuses that shape, so this is a defence in depth on the
     * immutable record rather than a boundary a caller can reach: nothing is
     * accepted at record time and rejected here. */
    if(first>=range_end || last>=range_end || last<first+2)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct ps5vk_operation *begin=&cb->operations[first]; VkRenderPass pass=begin->render_pass;
    /* Execute the shared-role profile: ordered subpasses using the
     * same color/depth attachments and layouts. Wider graphs, or graphs that
     * would need attachment rebinding/layout changes, remain fail-closed. */
    if(!pass->subpass_count || pass->subpass_count>PS5VK_MAX_SUBPASSES)return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct ps5vk_subpass *subpass=ps5vk_render_pass_subpass(pass,0);
    int depth=subpass->depth.attachment!=VK_ATTACHMENT_UNUSED;
    const uint32_t color_count=subpass->color_count;
    /* The roles are positional in this profile: a subpass names its colour
     * attachments first, in order, and the optional depth attachment after
     * them. A DEPTH-ONLY pass names no colour role at all, so its single
     * attachment IS the depth one and the colour half of this executor does
     * nothing. Every colour target must be a format this profile renders into,
     * and a pass that names no colour and no depth attachment has no target at
     * all. */
    if(color_count>PS5VK_MAX_COLOR_ATTACHMENTS ||
       (!color_count && !depth) ||
       pass->attachment_count!=color_count+(depth?1u:0u))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkFormat color_format[PS5VK_MAX_COLOR_ATTACHMENTS];
    for(uint32_t c=0;c<color_count;++c) {
        color_format[c]=begin->framebuffer->attachments[c]->image->info.format;
        if(subpass->color[c].attachment!=c ||
           (color_format[c]!=VK_FORMAT_B8G8R8A8_UNORM &&
            color_format[c]!=VK_FORMAT_R8G8B8A8_UNORM))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(depth && subpass->depth.attachment!=color_count)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for(uint32_t index=1;index<pass->subpass_count;++index) {
        const struct ps5vk_subpass *next=ps5vk_render_pass_subpass(pass,index);
        if(next->color_count!=color_count) return VK_ERROR_FEATURE_NOT_PRESENT;
        for(uint32_t c=0;c<color_count;++c)
            if(next->color[c].attachment!=subpass->color[c].attachment ||
               next->color[c].layout!=subpass->color[c].layout)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        if(next->depth.attachment!=subpass->depth.attachment ||
           next->depth.layout!=subpass->depth.layout)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* Every forward edge is covered by flushing CB and acquiring at each
     * intervening boundary, across all views. Self-dependencies only declare
     * allowed in-pass scopes; they do not schedule work by themselves. */
    for(uint32_t i=0;i<pass->dependency_count;++i) {
        const VkSubpassDependency *dep=&pass->dependencies[i];
        if(dep->srcSubpass>=pass->subpass_count ||
           dep->dstSubpass>=pass->subpass_count ||
           dep->srcSubpass>dep->dstSubpass)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* The views each subpass renders. The pass owns its multiview
     * configuration - the create-info pointers were never retained - and what
     * this executor needs from it is one mask per subpass. The ownership is
     * checked here again rather than trusted: the mask a subpass reads has to
     * be that subpass's, and a view-local dependency with a view offset moves
     * every view by that offset, which the single pass boundary acquires for
     * all views at once and cannot express. */
    const struct ps5vk_render_pass_multiview *multiview=&pass->multiview;
    if(multiview->present) {
        if(multiview->subpass_count!=pass->subpass_count ||
           multiview->dependency_count!=pass->dependency_count)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        for(uint32_t k=0;k<multiview->dependency_count;++k)
            if(multiview->view_offsets[k])return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct ps5vk_attachment_plan color_plan[PS5VK_MAX_COLOR_ATTACHMENTS]={{0}},depth_plan={0};
    /* One clear word per colour target: the ordered clear before the pass is a
     * whole-surface DMA fill per target, and two attachments may legitimately
     * ask for different values. */
    uint32_t clear_word[PS5VK_MAX_COLOR_ATTACHMENTS]={0};
    for(uint32_t c=0;c<color_count;++c) {
        if(ps5vk_attachment_plan(&pass->attachments[c],color_format[c],
            subpass->color[c].layout,VK_FALSE,&color_plan[c])!=VK_SUCCESS)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if(!color_plan[c].clear) continue;
        VkImage image=begin->framebuffer->attachments[c]->image;
        int clear_ok=color_format[c]==VK_FORMAT_B8G8R8A8_UNORM ?
            ps5vk_color_clear_bgra8(begin->clears[c].color.float32,&clear_word[c]) :
            ps5vk_color_clear_rgba8(begin->clears[c].color.float32,&clear_word[c]);
        if(image->info.format!=color_format[c] || begin->clear_count<=c || !clear_ok ||
           begin->render_area.offset.x || begin->render_area.offset.y ||
           begin->render_area.extent.width!=image->info.extent.width ||
           begin->render_area.extent.height!=image->info.extent.height)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(depth) {
        const VkAttachmentDescription *a=&pass->attachments[color_count];
        VkImage image=begin->framebuffer->attachments[color_count]->image;
        if(ps5vk_attachment_plan(a,VK_FORMAT_D32_SFLOAT,subpass->depth.layout,
            VK_TRUE,&depth_plan)!=VK_SUCCESS ||
            image->info.extent.width!=begin->framebuffer->width ||
            image->info.extent.height!=begin->framebuffer->height)return VK_ERROR_FEATURE_NOT_PRESENT;
        if(depth_plan.clear) {
            float clear=begin->clears[color_count].depthStencil.depth;
            if(begin->clear_count<=color_count || !(clear>=0 && clear<=1) ||
                begin->render_area.offset.x || begin->render_area.offset.y ||
                begin->render_area.extent.width!=begin->framebuffer->width ||
                begin->render_area.extent.height!=begin->framebuffer->height)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    /* Ordered body of the pass. vkCmdExecuteCommands carries no work of its
     * own: it NAMES children, and each name expands here into that child's own
     * recorded draws, taken from the child's own buffer. Nothing was flattened
     * into the primary at record time, so this is where the one command stream
     * the pass needs is assembled - and only here, from buffers the submission
     * itself names, matched by identity so this code cannot invent the
     * association. */
    const struct ps5vk_operation *body[PS5VK_MAX_OPERATIONS];
    unsigned body_count=0,next_buffer=1;
    for(unsigned i=first+1;i<last;++i) {
        const struct ps5vk_operation *op=&cb->operations[i];
        if(op->type==PS5VK_NEXT_SUBPASS || op->type==PS5VK_CLEAR_ATTACHMENT) {
            if(body_count==PS5VK_MAX_OPERATIONS)return VK_ERROR_FEATURE_NOT_PRESENT;
            body[body_count++]=op;
            continue;
        }
        if(op->type==PS5VK_EXECUTE_COMMANDS) {
            VkCommandBuffer const *children=(VkCommandBuffer const *)op->owned_payload;
            if(!children || !op->child_count ||
               op->owned_payload_size!=(size_t)op->child_count*sizeof(*children))
                return VK_ERROR_FEATURE_NOT_PRESENT;
            for(uint32_t n=0;n<op->child_count;++n) {
                if(next_buffer>=s->count || s->buffers[next_buffer]!=children[n])
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                VkCommandBuffer child=s->buffers[next_buffer];
                uint32_t child_first=ps5vk_submission_first_operation(s,next_buffer);
                uint32_t child_count=ps5vk_submission_operation_count(s,next_buffer);
                if(child->operation_count>PS5VK_MAX_OPERATIONS ||
                   child_first>child->operation_count ||
                   child_count>child->operation_count-child_first)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                for(uint32_t k=child_first;k<child_first+child_count;++k) {
                    const struct ps5vk_operation *inner=&child->operations[k];
                    if(inner->type!=PS5VK_DRAW && inner->type!=PS5VK_DRAW_INDEXED &&
                       !ps5vk_indirect_graphics_operation(inner->type))
                        return VK_ERROR_FEATURE_NOT_PRESENT;
                    /* The prepared-draw arena is bounded; refuse rather than
                     * silently dropping the tail of the pass. */
                    if(body_count==PS5VK_MAX_OPERATIONS)return VK_ERROR_FEATURE_NOT_PRESENT;
                    body[body_count++]=inner;
                }
                ++next_buffer;
            }
            continue;
        }
        if(op->type!=PS5VK_DRAW && op->type!=PS5VK_DRAW_INDEXED &&
           !ps5vk_indirect_graphics_operation(op->type))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if(body_count==PS5VK_MAX_OPERATIONS)return VK_ERROR_FEATURE_NOT_PRESENT;
        body[body_count++]=op;
    }
    /* Every named buffer must have been consumed by a name in this pass, and
     * the pass must actually carry work: naming only empty secondaries expands
     * to no draws at all, which is the zero-body shape recording already
     * refuses. Defence in depth behind that check and the submission-time one,
     * on the immutable record this backend is handed. */
    if(next_buffer!=s->count || !body_count)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct graphics_job *j=calloc(1,sizeof(*j)); if(!j)return VK_ERROR_OUT_OF_HOST_MEMORY;
    j->serial=s->serial;
    /* The image the prelude and postlude act on. A depth-only pass has no
     * colour image, and the helpers below already treat a missing one as
     * "no colour work in this range" rather than as an error. */
    j->color=color_count?begin->framebuffer->attachments[0]->image:NULL;
    phase="command-arena";
    VkResult rc=ps5vk_draw_batch_open(&j->chain,j->serial);
    if(rc==VK_ERROR_DEVICE_LOST)retain("command-create");
    if(rc!=VK_SUCCESS)goto fail;
    /* Every emission below writes through this cursor pair and asks the chain
     * for room first; the chain seals the open arena and opens the next one
     * when a whole emission would not fit. */
    uint32_t *cursor=j->chain.cursor,*end=j->chain.end;
#define BATCH_RESERVE(need) do { j->chain.cursor=cursor; \
        rc=ps5vk_draw_batch_reserve(&j->chain,(need)); if(rc!=VK_SUCCESS)goto fail; \
        cursor=j->chain.cursor;end=j->chain.end; } while(0)
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    /* Owned, cache-line-aligned, zeroed GPU-visible slot. Creation zeroes it,
     * which is what makes an unwritten pair read as unavailable. */
    phase="occlusion-slot";
    rc=ps5vk_command_arena_create(&j->slot);
    if(rc==VK_ERROR_DEVICE_LOST)retain("occlusion-slot-create");
    if(rc!=VK_SUCCESS)goto fail;
    /* The arena is zeroed by the CPU, and this memory is non-coherent: flush
     * the zeroed lines before the GPU writes the counter pair into them, or a
     * dirty CPU line could later overwrite the hardware's words. The readback
     * side flushes again only after the exact completion label. */
    cache(j->slot.address,(size_t)PS5VK_OCCLUSION_PROBE_BYTES);
    j->slot_active=1;
#endif
    /* Prelude transitions/clears precede attachment load operations in both
     * the command stream and the tentative layout transaction. */
    phase="prelude";
    rc=ps5vk_upload_commands(d,cb->operations+range_first,first-range_first,j->color,
        &j->layouts,&cursor,end,cache);
    if(rc!=VK_SUCCESS)goto fail;
    /* Record the scoped render-pass transitions transactionally. Resource
     * state becomes committed only after the exact GPU completion label. */
    phase="attachment-layout";
    /* Every colour target the pass carries takes its own initial-to-final
     * transition, in attachment order: a second target is a separate surface
     * with its own tracked layout, not part of attachment zero's. */
    for(uint32_t c=0;c<color_count;++c) {
        rc=ps5vk_layout_transition(&j->layouts,begin->framebuffer->attachments[c]->image,
            pass->attachments[c].initialLayout,pass->attachments[c].finalLayout);
        if(rc!=VK_SUCCESS)goto fail;
    }
    if(depth) {
        rc=ps5vk_layout_transition(&j->layouts,begin->framebuffer->attachments[color_count]->image,
            pass->attachments[color_count].initialLayout,pass->attachments[color_count].finalLayout);
        if(rc!=VK_SUCCESS)goto fail;
    }
    ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT];
    if(ps5_color_select_runtime_defaults(defaults,sceAgcGetRegisterDefaults())) {rc=VK_ERROR_INITIALIZATION_FAILED;goto fail;}
    /* One whole-surface fill per colour target that asked to be cleared, in
     * attachment order, each followed by the acquire that publishes it. The
     * targets are independent surfaces, so a second fill can neither observe
     * nor disturb the first. */
    for(uint32_t c=0;c<color_count;++c) {
        if(!color_plan[c].clear)continue;
        void *address;VkDeviceSize bytes;
        VkImage image=begin->framebuffer->attachments[c]->image;
        rc=ps5vk_image_span(d,image,&address,&bytes);
        if(rc!=VK_SUCCESS)goto fail;
        cache(address,(size_t)bytes);
        size_t n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)address,bytes,clear_word[c]);
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,"PS5VK_COLOR_CLEAR_PREPARED serial=%llu target=%u bgra=%08x bytes=%llu",
            (unsigned long long)j->serial,c,clear_word[c],(unsigned long long)bytes);
    }
    if(depth && depth_plan.clear) {
        void *address;VkDeviceSize bytes;
        rc=ps5vk_image_span(d,begin->framebuffer->attachments[color_count]->image,&address,&bytes);
        if(rc!=VK_SUCCESS)goto fail;
        uint32_t value;
        memcpy(&value,&begin->clears[color_count].depthStencil.depth,sizeof(value));
        cache(address,(size_t)bytes);
        size_t n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)address,bytes,value);
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
    }
    phase="draw";
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    if(j->slot_active) {
        size_t n=ps5vk_graphics_occlusion_event(cursor,(size_t)(end-cursor),(uint64_t)(uintptr_t)j->slot.address);
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=1;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_OCCLUSION_PROBE_BEGIN serial=%llu base=%llx pairs=%u",
            (unsigned long long)j->serial,(unsigned long long)(uintptr_t)j->slot.address,
            (unsigned)PS5VK_OCCLUSION_PROBE_PAIRS);
    }
#endif
    uint32_t subpass_index=0;
    for(unsigned i=0;i<body_count;++i) {
        const struct ps5vk_operation *recorded=body[i];
        if(recorded->type==PS5VK_CLEAR_ATTACHMENT) {
            if(recorded->subpass!=subpass_index || !ps5vk_clear_attachment_valid(recorded)) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=2;goto fail;
            }
            VkImageView view=recorded->framebuffer->attachments[
                ps5vk_render_pass_subpass(pass,subpass_index)->color[0].attachment];
            void *address;VkDeviceSize bytes,stride;
            rc=ps5vk_image_span(d,view->image,&address,&bytes);
            if(rc!=VK_SUCCESS)goto fail;
            rc=ps5vk_native_layer_footprint(d,view->image,&stride);
            if(rc!=VK_SUCCESS || !stride || stride>bytes/view->image->info.arrayLayers) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=3;goto fail;
            }
            /* The final reserved tail word of the arena that EXECUTES this
             * clear is a private intra-submission token, never the label that
             * poll() treats as GPU completion. The whole clear sequence is
             * kept inside one arena so the wait and the token it names cannot
             * straddle a chain boundary. */
            BATCH_RESERVE(PS5VK_DRAW_BATCH_INITIAL_RESERVE*2u);
            size_t n=ps5vk_graphics_release_wait(cursor,(size_t)(end-cursor),
                (uintptr_t)(ps5vk_draw_batch_open_label(&j->chain)+7),i+1u);
            if(!n){rc=VK_ERROR_UNKNOWN;draw_site=4;goto fail;}cursor+=n;
            n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
            if(!n){rc=VK_ERROR_UNKNOWN;draw_site=5;goto fail;}cursor+=n;
            VkDeviceSize offset=(VkDeviceSize)view->range.baseArrayLayer*stride;
            n=ps5vk_color_rect_clear(cursor,(size_t)(end-cursor),
                (uintptr_t)address+offset,bytes-offset,(size_t)stride,
                view->image->info.extent.width,view->image->info.extent.height,
                view->range.layerCount,multiview->present?multiview->view_masks[subpass_index]:0,
                recorded->clear_rect.rect,recorded->clear_word);
            if(!n){rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=6;goto fail;}cursor+=n;
            n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
            if(!n){rc=VK_ERROR_UNKNOWN;draw_site=7;goto fail;}cursor+=n;
            continue;
        }
        if(recorded->type==PS5VK_NEXT_SUBPASS) {
            /* The boundary names the subpass it enters: a record that names any
             * other one cannot say which subpass's view mask the draws that
             * follow belong to. The bound matters as much as the step, because
             * the subpass it names indexes the pass's owned mask array - a
             * record that names the pass's own subpass count or beyond is
             * refused before anything is read. */
            if(recorded->subpass!=subpass_index+1u ||
               recorded->subpass>=pass->subpass_count) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=8;goto fail;
            }
            subpass_index=recorded->subpass;
            BATCH_RESERVE(PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WORDS+PS5VK_GRAPHICS_ACQUIRE_WORDS);
            const struct ps5vk_subpass *next_subpass=
                ps5vk_render_pass_subpass(pass,subpass_index);
            if(next_subpass->input_count || pass->dependency_count) {
                size_t color_barrier=ps5vk_graphics_color_to_texture(
                    cursor,(size_t)(end-cursor));
                if(!color_barrier){rc=VK_ERROR_UNKNOWN;draw_site=9;goto fail;}
                cursor+=color_barrier;
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_COLOR_TO_TEXTURE_BARRIER serial=%llu subpass=%u words=%zu",
                    (unsigned long long)j->serial,recorded->subpass,color_barrier);
            }
            size_t boundary=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
            if(!boundary){rc=VK_ERROR_UNKNOWN;draw_site=10;goto fail;}
            cursor+=boundary;
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_SUBPASS_BOUNDARY serial=%llu subpass=%u words=%zu",
                (unsigned long long)j->serial,recorded->subpass,boundary);
            continue;
        }
        /* An indirect operation expands into indirect_count commands in the
         * recorded order. Pass one resolves and validates every command at
         * the queue head (bounds, feature gates, the exact argument bytes) and
         * finds the first command that draws anything: the prepared draw -
         * pipeline, viewport, descriptor tables, the full vertex-buffer span -
         * is shaped by that command and shared by all of them, and it is the
         * only per-operation allocation, whatever the count. Pass two, below,
         * re-resolves each command in order and emits it with its own
         * DrawIndex, index range and vertex-range check. A command that draws
         * nothing still consumes its index. */
        struct ps5vk_operation resolved;
        const struct ps5vk_operation *op=recorded;
        const int indirect=ps5vk_indirect_graphics_operation(recorded->type);
        uint32_t command_count=1u;
        if(indirect) {
            command_count=recorded->indirect_count;
            uint32_t shape=0u;int shaped=0;
            for(uint32_t k=0;k<command_count;++k) {
                rc=ps5vk_indirect_resolve_command(d,recorded,k,&resolved);
                if(rc!=VK_SUCCESS)goto fail;
                if(!shaped && draw_has_work(&resolved)){shape=k;shaped=1;}
            }
            if(command_count)rc=ps5vk_indirect_resolve_command(d,recorded,shape,&resolved);
            else rc=ps5vk_indirect_resolve(d,recorded,&resolved);
            if(rc!=VK_SUCCESS)goto fail;
            op=&resolved;
        }
        struct ps5vk_prepared_draw *draw=&j->draws[j->count];
        struct ps5vk_native_graphics_pipeline *p=op->pipeline->graphics_state;
        if(!p || !p->pair || !p->pair->ready){rc=VK_ERROR_UNKNOWN;draw_site=11;goto fail;}
        if(p->pair->tess_rings) {
            const uint32_t *table=p->pair->tess_rings;
            uint64_t address=((uint64_t)table[21]<<32)|table[20];
            if(!address || (address&255u) || table[22]<65536u*4u ||
                (j->ring_address && j->ring_address!=address)) {
                ps5log_printf(PS5LOG_ERR,"PS5VK_TESS_RING_PREPARE_INVALID serial=%llu present=%u aligned=%u bytes=%u consistent=%u",
                    (unsigned long long)j->serial,(unsigned)(address!=0),
                    (unsigned)((address&255u)==0),table[22],
                    (unsigned)(!j->ring_address || j->ring_address==address));
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=30;goto fail;
            }
            j->ring_address=address;
            /* Validate the owned offchip allocation described by our
             * table, bounded strictly before the owned factor ring. */
            uint64_t offchip=((uint64_t)table[25]<<32)|table[24];
            if(offchip==(uint64_t)(uintptr_t)table+256u &&
               offchip<address && table[26]==address-offchip) {
                j->offchip_bytes=table[26];
            }
#if defined(PS5VK_TESS_HULL_TRACE) && PS5VK_TESS_HULL_TRACE
            uint64_t trace=((uint64_t)table[29]<<32)|table[28];
            if(trace!=address+table[22] || table[30]!=8192u ||
               (j->hull_trace && (uintptr_t)j->hull_trace!=trace)) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=30;goto fail;
            }
            j->hull_trace=(uint32_t *)(uintptr_t)trace;
#endif
        }
        /* Check each sampled resource, not just element zero of set zero.
         * Preparation validates generations and copies exactly these tables. */
        if(op->pipeline->set_count>PS5VK_MAX_SETS){rc=VK_ERROR_UNKNOWN;draw_site=12;goto fail;}
        /* The one-input profile: how many input-attachment bindings the whole
         * pipeline declares. The gate refuses anything but exactly one, so a
         * second attachment can never be half-served. */
        uint32_t input_bindings=0;
        if(p->pair->runtime_arguments.enabled)
            for(unsigned s=0;s<op->pipeline->set_count;++s)
                for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b)
                    if(op->pipeline->sets[s].binding[b].count &&
                       op->pipeline->sets[s].type[b]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
                        ++input_bindings;
        for(unsigned set_index=0;set_index<op->pipeline->set_count;++set_index) {
            if(p->pair->runtime_arguments.enabled &&
               !ps5vk_runtime_resource_bindings(&p->pair->runtime_arguments,
                   &p->pair->hull_arguments,set_index))continue;
            VkDescriptorSet set=op->sets[set_index];
            if(!set || !set->pool || set->pool->device!=d ||
               set->generation!=op->generations[set_index] ||
               memcmp(&set->signature,&op->pipeline->sets[set_index],sizeof(set->signature))) {
                rc=VK_ERROR_UNKNOWN;draw_site=13;goto fail;
            }
            for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
                const struct ps5vk_binding *binding=&set->signature.binding[b];
                const VkDescriptorType type=set->signature.type[b];
                const int buffer_type=ps5vk_graphics_buffer_type(type);
                const int input_type=type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
                if(binding->count && type!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                   !buffer_type && !input_type) {
                    rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=14;goto fail;
                }
                if(binding->first>PS5VK_MAX_DESCRIPTORS ||
                   binding->count>PS5VK_MAX_DESCRIPTORS-binding->first){rc=VK_ERROR_UNKNOWN;draw_site=15;goto fail;}
                /* Only the bindings the compiled stages dereference are a
                 * requirement; the table is shared, so the two stage masks are
                 * ORed. A zero mask keeps the whole declaration (fail closed). */
                if(p->pair->runtime_arguments.enabled) {
                    const uint64_t used=ps5vk_runtime_resource_bindings(
                        &p->pair->runtime_arguments,&p->pair->hull_arguments,set_index);
                    if(used && binding->count && !(used&(UINT64_C(1)<<b)))continue;
                }
                for(unsigned e=0;e<binding->count;++e) {
                    unsigned index=binding->first+e;
                    if(!set->defined[index]){rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=16;goto fail;}
                    if(buffer_type) {
                        /* The encoder resolves and validates the base address;
                         * a null handle must never be encoded. */
                        if(!set->buffers[index].buffer) {
                            rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=17;goto fail;
                        }
                        continue;
                    }
                    if(input_type) {
                        /* An input attachment is not a sampled resource: it is
                         * the subpass's own framebuffer view read at GENERAL
                         * through the resource-only record, and the bounded
                         * one-input rule lives in one place so the driver and
                         * its tests decide the same shape. */
                        rc=ps5vk_input_attachment_gate(d,op->render_pass,subpass_index,
                            op->framebuffer,set,binding,b,type,index,input_bindings,
                            &p->pair->runtime_arguments,set_index);
                        if(rc!=VK_SUCCESS)goto fail;
                        continue;
                    }
                    if(!set->image_resources[index] ||
                       set->images[index].imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                        rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=18;goto fail;
                    }
                    rc=ps5vk_layout_require(&j->layouts,set->image_resources[index],set->images[index].imageLayout);
                    if(rc!=VK_SUCCESS)goto fail;
                }
            }
        }
        struct ps5vk_index_fetch indices={0};
        if(op->type==PS5VK_DRAW_INDEXED) {
            /* Runtime shaders can generate positions from gl_VertexIndex
             * without vertex attributes. Index fetch and LS baseVertex are
             * independent of a vertex-buffer table. Keep the legacy gate. */
            if(!op->pipeline->vertex_binding_count && !p->pair->runtime_arguments.enabled) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=19;goto fail;
            }
            rc=ps5vk_index_fetch_prepare(d,op,&indices);if(rc!=VK_SUCCESS)goto fail;
        }
        const uint32_t vertex_usage=p->pair->runtime_arguments.enabled?
            (p->pair->runtime_arguments.vertex_buffer_usage_mask |
             p->pair->hull_arguments.vertex_buffer_usage_mask):
            (op->pipeline->vertex_binding_count?1u:0u);
        const struct ps5vk_graphics_key key={.vertex_binding_count=op->pipeline->vertex_binding_count,
            .vertex_attribute_count=op->pipeline->vertex_attribute_count,
            .vertex_bindings=op->pipeline->vertex_bindings,.vertex_attributes=op->pipeline->vertex_attributes};
        if(vertex_usage) {
            rc=ps5vk_native_prepare_vertex_draw_masked(d,op,&begin->render_area,defaults,&key,
                (uintptr_t)p->pair,vertex_usage,draw);
        } else rc=ps5vk_native_prepare_resource_draw(d,op,&begin->render_area,defaults,(uintptr_t)p->pair,draw);
        if(rc!=VK_SUCCESS) {
            ps5log_printf(PS5LOG_ERR,"PS5VK_DRAW_RESOURCE_PREPARE_FAILED serial=%llu vertex_mask=%x bindings=%u attributes=%u rc=%d",
                (unsigned long long)j->serial,vertex_usage,key.vertex_binding_count,
                key.vertex_attribute_count,(int)rc);
            draw_site=31;goto fail;
        }
        ++j->count;
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 4
        if(j->serial==17 && draw->vertex_table && vertex_usage==1u &&
           op->type==PS5VK_DRAW && op->vertex_count==80u &&
           key.vertex_binding_count==1u && key.vertex_bindings[0].stride==4u) {
            void *input=NULL;VkDeviceSize input_bytes=0;
            if(ps5vk_buffer_span(d,op->vertices[0].buffer,op->vertices[0].offset,
                    VK_WHOLE_SIZE,&input,&input_bytes)==VK_SUCCESS && input_bytes>=320u) {
                uint32_t v[80];memcpy(v,input,sizeof(v));
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TESS_VERTEX_SOURCE serial=17 bytes=%llu first=%u table=%08x,%08x,%08x,%08x x=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                    (unsigned long long)input_bytes,op->first_vertex,
                    draw->vertex_table[0],draw->vertex_table[1],draw->vertex_table[2],draw->vertex_table[3],
                    v[0],v[10],v[20],v[30],v[40],v[50],v[60],v[70]);
            }
        }
#endif
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE && PS5VK_GRAPHICS_SCISSOR_PROBE!=15
        if(PS5VK_GRAPHICS_SCISSOR_PROBE==13)
            ps5log_printf(PS5LOG_MARK,"PS5VK_BINDINGS_PREPARED serial=%llu mask=%04x copied_bytes=%zu",
                (unsigned long long)j->serial,vertex_usage,draw->vertex_bounce_bytes);
        if(draw->vertex_bounce)
            ps5log_printf(PS5LOG_MARK,"PS5VK_VERTEX_BOUNCE serial=%llu draw=%u bytes=%zu alignment=4",
                (unsigned long long)j->serial,j->count-1,draw->vertex_bounce_bytes);
        /* Private TCP evidence of the exact prepared block, not inferred API
         * intent. Include duplicate offsets and order; later writes can win. */
        for(unsigned k=0;k<draw->state->cx_count;++k)
            ps5log_printf(PS5LOG_MARK,"PS5VK_STATE_CX serial=%llu draw=%u index=%u offset=%x value=%08x",
                (unsigned long long)j->serial,j->count-1,k,
                draw->state->cx[k].offset,draw->state->cx[k].value);
        for(unsigned k=0;k<12;++k)
            ps5log_printf(PS5LOG_MARK,"PS5VK_STATE_SH serial=%llu draw=%u index=%u offset=%x value=%08x",
                (unsigned long long)j->serial,j->count-1,k,
                draw->state->sh[k].offset,draw->state->sh[k].value);
#endif
        if(!p->global_table){rc=VK_ERROR_UNKNOWN;draw_site=20;goto fail;}
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE && PS5VK_GRAPHICS_SCISSOR_PROBE!=15
        if(draw->texture_table)
            for(unsigned k=0;k<12;++k)
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TEXTURE_DESCRIPTOR serial=%llu draw=%u word=%u value=%08x",
                    (unsigned long long)j->serial,j->count-1,k,draw->texture_table[k]);
#endif
        /* A subpass with a view mask renders the same draw once per view of
         * that mask, into that view's own layer. The whole expansion - the
         * ascending views, every view's colour and depth layer, and the words
         * each layer moves - is resolved before the first word of this draw is
         * emitted, so a view the attachment cannot address is refused with
         * nothing written for that draw (and, on any failure here, the whole
         * unsubmitted job is discarded). viewMask == 0 keeps exactly the single
         * draw and target the profile has always emitted. */
        const uint32_t view_mask=multiview->present?multiview->view_masks[subpass_index]:0u;
        struct ps5vk_view_emit view_emit[PS5VK_MAX_VIEW_MASK_VIEWS];
        struct ps5vk_target_registers view_prepared_color,view_prepared_depth;
        struct ps5vk_target_registers view_color[PS5VK_MAX_VIEW_MASK_VIEWS];
        struct ps5vk_target_registers view_depth[PS5VK_MAX_VIEW_MASK_VIEWS];
        uint32_t view_batch=1;
        if(view_mask) {
            /* The ViewIndex only exists in compiler metadata: without it the
             * profile cannot tell whether a stage reads the built-in and has no
             * slot to deliver the value through, so a multiview subpass runs on
             * the metadata ABI or it does not run. */
            if(!p->pair->runtime_arguments.enabled){rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=21;goto fail;}
            /* The whole view expansion below rewrites ONE colour target's
             * layer-addressed words. A subpass that names more than one colour
             * attachment would need the same expansion per attachment, which is
             * not served, so it is refused here rather than expanded for the
             * first target only. */
            {
                const struct ps5vk_subpass *viewed=ps5vk_render_pass_subpass(pass,subpass_index);
                if(!viewed || viewed->color_count!=1u){rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=22;goto fail;}
            }
            uint32_t view_indices[PS5VK_MAX_VIEW_MASK_VIEWS],view_count=0;
            rc=ps5vk_native_view_expand(view_mask,view_indices,PS5VK_MAX_VIEW_MASK_VIEWS,&view_count);
            if(rc!=VK_SUCCESS)goto fail;
            VkFramebuffer fb=op->framebuffer;
            /* The target the draw was prepared with is layer zero of the same
             * builder, so the emission can carry only what a layer moves. */
            rc=ps5vk_native_layer_target(d,fb->attachments[fb->color_attachments[0]],0u,
                defaults,&view_prepared_color);
            if(rc!=VK_SUCCESS)goto fail;
            if(depth) {
                rc=ps5vk_native_layer_target(d,fb->attachments[fb->depth_attachment],0u,
                    NULL,&view_prepared_depth);
                if(rc!=VK_SUCCESS)goto fail;
            }
            for(uint32_t v=0;v<view_count;++v) {
                rc=ps5vk_native_view_layer_target(d,fb->attachments[fb->color_attachments[0]],
                    view_indices[v],defaults,&view_color[v]);
                if(rc!=VK_SUCCESS)goto fail;
                if(depth) {
                    rc=ps5vk_native_view_layer_target(d,fb->attachments[fb->depth_attachment],
                        view_indices[v],NULL,&view_depth[v]);
                    if(rc!=VK_SUCCESS)goto fail;
                }
                view_emit[v]=(struct ps5vk_view_emit){.view_index=view_indices[v],
                    .prepared_color=&view_prepared_color,.view_color=&view_color[v],
                    .prepared_depth=depth?&view_prepared_depth:NULL,
                    .view_depth=depth?&view_depth[v]:NULL};
            }
            view_batch=view_count;
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_VIEW_EXPANSION serial=%llu subpass=%u mask=%08x views=%u",
                (unsigned long long)j->serial,subpass_index,view_mask,view_count);
        }
        uint32_t tables[PS5VK_RUNTIME_DESCRIPTOR_SETS]={0};
        if(p->pair->runtime_arguments.enabled)
            for(unsigned s=0;s<PS5VK_RUNTIME_DESCRIPTOR_SETS;++s)
                tables[s]=(uint32_t)(uintptr_t)draw->descriptor_tables[s];
        uint32_t emitted_commands=0,emitted_draws=0;
        for(uint32_t k=0;k<(command_count?command_count:1u);++k) {
            if(indirect && command_count) {
                rc=ps5vk_indirect_resolve_command(d,recorded,k,&resolved);
                if(rc!=VK_SUCCESS)goto fail;
                op=&resolved;
                /* DrawIndex k is consumed whether or not the command draws. */
                if(!draw_has_work(op))continue;
                if(op->type==PS5VK_DRAW_INDEXED) {
                    rc=ps5vk_index_fetch_prepare(d,op,&indices);if(rc!=VK_SUCCESS)goto fail;
                }
                /* The shared vertex table spans the whole bound buffers; this
                 * command's own firstVertex+vertexCount must still lie inside
                 * them, judged by the same code that judged the shaping one. */
                if(vertex_usage) {
                    struct ps5vk_vertex_fetch_table check;
                    rc=ps5vk_vertex_fetch_used_spans(d,&key,op,vertex_usage,&check);
                    if(rc!=VK_SUCCESS)goto fail;
                }
            }
            for(uint32_t v=0;v<view_batch;++v) {
                /* Room for one whole emission, measured from the largest one
                 * seen so far; a shortfall seals the open arena behind the
                 * previous emission and continues in the next one. */
                BATCH_RESERVE(j->chain.reserve);
                uint32_t *emission=cursor;
                unsigned attempt=0;
                for(;;) {
                    if(p->pair->runtime_arguments.enabled) {
                        rc=ps5vk_native_emit_runtime_draw(&cursor,(uint32_t)(end-cursor),draw->state,
                            draw->state,draw->bytes,op,(uint32_t)(uintptr_t)draw->vertex_table,tables,
                            view_mask?&view_emit[v]:NULL,
                            op->type==PS5VK_DRAW_INDEXED?&indices:NULL,sceAgcDcbDrawIndex);
                    } else if(vertex_usage) {
                        if(!draw->vertex_table){rc=VK_ERROR_UNKNOWN;draw_site=22;goto fail;}
                        if(op->pipeline->set_count) {
                            if(!draw->texture_table){rc=VK_ERROR_UNKNOWN;draw_site=23;goto fail;}
                            rc=ps5vk_native_emit_textured_draw(&cursor,(uint32_t)(end-cursor),draw->state,draw->state,draw->bytes,op,
                                (uint32_t)(uintptr_t)p->global_table,(uint32_t)(uintptr_t)draw->vertex_table,
                                (uint32_t)(uintptr_t)draw->texture_table,op->type==PS5VK_DRAW_INDEXED?&indices:NULL,sceAgcDcbDrawIndex);
                        } else if(op->type==PS5VK_DRAW_INDEXED)
                            rc=ps5vk_native_emit_indexed_draw(&cursor,(uint32_t)(end-cursor),draw->state,draw->state,draw->bytes,op,
                                (uint32_t)(uintptr_t)p->global_table,(uint32_t)(uintptr_t)draw->vertex_table,&indices,sceAgcDcbDrawIndex);
                        else rc=ps5vk_native_emit_vertex_draw(&cursor,(uint32_t)(end-cursor),draw->state,draw->state,draw->bytes,op,
                            (uint32_t)(uintptr_t)p->global_table,(uint32_t)(uintptr_t)draw->vertex_table);
                    } else rc=ps5vk_native_emit_draw(&cursor,(uint32_t)(end-cursor),draw->state,draw->state,draw->bytes,op,
                            (uint32_t)(uintptr_t)p->global_table);
                    if(rc==VK_SUCCESS)break;
                    /* The emitters advance the cursor only on full success.
                     * A first emission larger than any measured one can still
                     * run out of arena; the open arena then holds only
                     * complete emissions, so it is sealed and this one is
                     * retried once in a fresh arena. A second failure is the
                     * emitter's own refusal and fails the job. */
                    if(attempt++ || cursor!=emission ||
                       emission==(uint32_t *)j->chain.arenas[j->chain.count-1].address+PS5VK_GRAPHICS_ACQUIRE_WORDS)goto fail;
                    j->chain.cursor=cursor;
                    VkResult retry_rc=ps5vk_draw_batch_retry(&j->chain,&emission);
                    if(retry_rc!=VK_SUCCESS){rc=retry_rc;goto fail;}
                    cursor=j->chain.cursor;end=j->chain.end;
                }
                if(cursor!=emission) {
                    ps5vk_draw_batch_measured(&j->chain,(uint32_t)(cursor-emission));
                    ++emitted_draws;
                }
            }
            ++emitted_commands;
        }
        if(indirect && command_count>1u)
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_MULTI_DRAW_EXPANDED serial=%llu body=%u commands=%u drawing=%u draws=%u arenas=%u",
                (unsigned long long)j->serial,i,command_count,emitted_commands,emitted_draws,j->chain.count);
    }
    /* A pass must have reached its last subpass: recording refuses to end one
     * early and the submission layer refuses it independently, so a body that
     * never entered the last subpass cannot be executed without dropping it. */
    if(subpass_index+1u!=pass->subpass_count) {rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=24;goto fail;}
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    if(j->slot_active) {
        size_t n=ps5vk_graphics_occlusion_event(cursor,(size_t)(end-cursor),
            (uint64_t)(uintptr_t)j->slot.address+8);
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=25;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_OCCLUSION_PROBE_END serial=%llu base_plus_8=%llx",
            (unsigned long long)j->serial,(unsigned long long)(uintptr_t)j->slot.address+8);
    }
#endif
    phase="postlude";
    BATCH_RESERVE(PS5VK_DRAW_BATCH_INITIAL_RESERVE*2u);
    if(last+1<range_end) {
        const struct ps5vk_operation *postlude=cb->operations+last+1;
        const unsigned postlude_count=range_end-last-1;
        unsigned readback=0;
        for(unsigned k=0;k<postlude_count;++k)
            readback|=postlude[k].type==PS5VK_COPY_IMAGE_BUFFER;
        if(readback) {
            struct ps5vk_readback_partition partition={0};
            struct ps5vk_readback_plan plan={0};
            rc=ps5vk_readback_partition(postlude,postlude_count,&partition);
            if(rc==VK_SUCCESS && partition.prefix_count)
                rc=ps5vk_upload_commands(d,postlude,partition.prefix_count,j->color,
                    &j->layouts,&cursor,end,cache);
            if(rc==VK_SUCCESS)
                rc=ps5vk_readback_commands(d,postlude+partition.readback_first,
                    partition.readback_count,j->color,&j->layouts,&plan);
            if(rc!=VK_SUCCESS)goto fail;
            j->readback_image=plan.image;j->readback_buffer=plan.buffer;
            j->readback_stride=plan.layer_stride;
        } else {
            rc=ps5vk_upload_commands(d,postlude,postlude_count,j->color,
                &j->layouts,&cursor,end,cache);
            if(rc!=VK_SUCCESS)goto fail;
        }
    }
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE && PS5VK_GRAPHICS_SCISSOR_PROBE!=15
    _Static_assert(8+PS5VK_GRAPHICS_PROBE_REGISTERS*4<=64,"probe must not overlap command words or leave reserved tail");
    BATCH_RESERVE(PS5VK_GRAPHICS_PROBE_WORDS+16u);
    uint32_t *probe=(uint32_t *)(ps5vk_draw_batch_open_label(&j->chain)+1);
    for(unsigned i=0;i<PS5VK_GRAPHICS_PROBE_REGISTERS;++i)probe[i]=0xd15ea5e0u+i;
    size_t probe_words=ps5vk_graphics_register_probe(cursor,(size_t)(end-cursor),(uintptr_t)probe);
    if(!probe_words){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=probe_words;
#endif
    phase="release-packet";
#if defined(PS5VK_TESS_END_VS_FLUSH) && PS5VK_TESS_END_VS_FLUSH
    /* Diagnostic only: wait for pre-raster shader work before the ordinary
     * release/label and native ring restoration. This tests ordering, not a
     * claim that every tessellation draw requires this extra event. */
    BATCH_RESERVE(2u);
    *cursor++=0xc0004600u;
    *cursor++=0x0000040fu; /* VS_PARTIAL_FLUSH, event index4 */
#endif
    j->chain.cursor=cursor;
    rc=ps5vk_draw_batch_close(&j->chain);
    if(rc!=VK_SUCCESS)goto fail;
#undef BATCH_RESERVE
    j->words=0;
    for(unsigned b=0;b<j->chain.count;++b)j->words+=j->chain.words[b];
#if defined(PS5VK_TESS_STATE_DUMP) && PS5VK_TESS_STATE_DUMP
    /* The COMMAND WORDS of a patch submission, which is the last stream in
     * this driver that has never been read.
     *
     * Dumping the built register banks found a register the pipeline never
     * emitted; the banks are still one level above what the GPU executes.
     * These words are the packets themselves - the register writes the banks
     * turn into, their counts, and the draw packet - so a register that the
     * bank carries but the encoder drops, or a draw packet with the wrong
     * count, is visible here and nowhere else.
     *
     * Gated on a patch draw rather than on every job: a tessellation draw
     * emits VGT_LS_HS_CONFIG at cx 0x2d6 and nothing else in this profile
     * does, so the nineteen geometry submissions in the same process stay
     * silent and the record count stays bounded. */
    {
        unsigned patch=0;
        for(unsigned k=0;k<j->count && !patch;++k) {
            const struct ps5vk_draw_state *st=j->draws[k].state;
            if(!st)continue;
            for(unsigned i=0;i<st->cx_count;++i)
                if(st->cx[i].offset==0x2d6u){patch=1;break;}
        }
        if(patch) {
            for(unsigned b=0;b<j->chain.count;++b) {
                const uint32_t *w=(const uint32_t *)j->chain.arenas[b].address;
                const uint32_t n=j->chain.words[b];
                if(!w)continue;
                for(uint32_t i=0;i<n;i+=4)
                    ps5log_printf(PS5LOG_MARK,
                        "PS5VK_TESS_PM4 serial=%llu arena=%u at=%u of=%u "
                        "%08x %08x %08x %08x",
                        (unsigned long long)j->serial,b,i,n,
                        w[i],i+1<n?w[i+1]:0u,i+2<n?w[i+2]:0u,
                        i+3<n?w[i+3]:0u);
            }
        }
    }
#endif
    *out=j;
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_PREPARED serial=%llu draws=%u words=%u",(unsigned long long)j->serial,j->count,j->words);
    if(j->chain.count>1u)
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_BATCHES serial=%llu arenas=%u words=%u",
            (unsigned long long)j->serial,j->chain.count,j->words);
    return VK_SUCCESS;
fail:
    ps5log_printf(PS5LOG_ERR,
        "PS5VK_GRAPHICS_PREPARE_FAILED serial=%llu phase=%s site=%u rc=%d",
        (unsigned long long)j->serial,phase,draw_site,rc);
    release(d,j);return rc;
}
/* Submit arena `index` of the chain. The first arena keeps the historic
 * PS5VK_GRAPHICS_SUBMIT / SUSPEND_POINT lines, exactly one pair per job, so
 * every verifier that pairs a submit with a completion still sees one of each;
 * later arenas report under their own names. */
static VkResult launch_batch(struct graphics_job *j,unsigned index)
{
    if(index>=j->chain.count || index!=j->launched)return VK_ERROR_DEVICE_LOST;
    struct ps5vk_command_arena *a=&j->chain.arenas[index];
    cache(a->address,PS5VK_COMMAND_ARENA_BYTES);
    struct ps5_agc_submit packet={a->address,j->chain.words[index],0,{0,0,0}};
    j->start=now(NULL); if(!j->start)return VK_ERROR_DEVICE_LOST;
    if(!index && j->ring_address) {
        void *expected=NULL;
        if(!__atomic_compare_exchange_n(&ring_owner,&expected,j,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return VK_ERROR_DEVICE_LOST;
        const struct ps5vk_tess_ring_ops ops={NULL,ring_get,ring_set};
        int rc=ps5vk_tess_ring_bind(&j->ring,&ops,j->ring_address,65536u);
        /* Private diagnostic: report kernel-owned state separately from PM4
         * intent. Raw getter fields have no inferred units or semantics. */
        uint16_t offchip_first=UINT16_MAX,offchip_second=UINT16_MAX;
        int offchip_rc=sceAgcDriverGetHsOffchipParam(&offchip_first,&offchip_second);
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_TESS_QUEUE_RING_STATE serial=%llu previous=%llx previous_raw_size=%u requested=%llx requested_raw_size=65536 offchip_rc=%d offchip_first=%u offchip_second=%u",
            (unsigned long long)j->serial,(unsigned long long)j->ring.previous_address,
            j->ring.previous_size,(unsigned long long)j->ring_address,
            offchip_rc,(unsigned)offchip_first,(unsigned)offchip_second);
        ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_QUEUE_RING_BOUND serial=%llu rc=%d state=%u",
            (unsigned long long)j->serial,rc,(unsigned)j->ring.state);
        if(rc) {
            if(j->ring.state!=PS5VK_TF_IDLE)retain("ring-bind-unresolved");
            __atomic_store_n(&ring_owner,NULL,__ATOMIC_RELEASE);
            return VK_ERROR_DEVICE_LOST;
        }
        const struct ps5vk_hs_ops hs_ops={NULL,offchip_get,offchip_set};
        int hs_rc=ps5vk_hs_bind(&j->offchip,&hs_ops,j->offchip_bytes);
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_TESS_OFFCHIP_BOUND serial=%llu rc=%d previous=%u,%u bytes=%u requested=0,%u state=%u",
            (unsigned long long)j->serial,hs_rc,j->offchip.previous_first,
            j->offchip.previous_second,j->offchip_bytes,
            j->offchip_bytes/32768u-1u,(unsigned)j->offchip.state);
        if(hs_rc) {
            if(j->offchip.state!=PS5VK_HS_IDLE)retain("offchip-bind-unresolved");
            if(ps5vk_tess_ring_restore(&j->ring))retain("ring-bind-rollback-unresolved");
            __atomic_store_n(&ring_owner,NULL,__ATOMIC_RELEASE);
            return VK_ERROR_DEVICE_LOST;
        }
        if(ps5vk_hs_submitting(&j->offchip))retain("offchip-submit-state");
        if(ps5vk_tess_ring_submitting(&j->ring))retain("ring-submit-state");
#if defined(PS5VK_TESS_HULL_TRACE) && PS5VK_TESS_HULL_TRACE
        if(j->hull_trace) {
            memset(j->hull_trace,0,8192u);
            cache(j->hull_trace,8192u);
        }
#endif
    }
    j->attempted=1;
    struct ps5vk_submit_result result=ps5vk_submit_suspend(&packet);
    int rc=result.submit_rc;
    if(!index)ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_SUBMIT serial=%llu rc=%d",(unsigned long long)j->serial,rc);
    else ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_BATCH_SUBMIT serial=%llu arena=%u arenas=%u words=%u rc=%d",
        (unsigned long long)j->serial,index,j->chain.count,j->chain.words[index],rc);
    if (result.suspend_attempted) {
        rc=result.suspend_rc;
        if(!index)ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_SUSPEND_POINT serial=%llu rc=%d",(unsigned long long)j->serial,rc);
        else ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_BATCH_SUSPEND_POINT serial=%llu arena=%u rc=%d",
            (unsigned long long)j->serial,index,rc);
    }
    if(rc)return VK_ERROR_DEVICE_LOST;
    j->launched=index+1u;
    return VK_SUCCESS;
}
static VkResult launch(VkDevice d,void *opaque)
{ (void)d; return launch_batch(opaque,0); }
static VkResult poll(VkDevice d,void *opaque,uint64_t *completed)
{
    struct graphics_job *j=opaque;*completed=0;
    if(j->complete){*completed=j->serial;return VK_SUCCESS;}
    if(!j->launched || j->launched>j->chain.count)return VK_ERROR_DEVICE_LOST;
    volatile uint64_t *label=ps5vk_command_arena_label(&j->chain.arenas[j->launched-1]);
    if(!label)return VK_ERROR_DEVICE_LOST;
    cache((const void *)label,8);uint64_t value=__atomic_load_n(label,__ATOMIC_ACQUIRE);
    if(value==j->serial && j->launched<j->chain.count) {
        /* This arena's draws retired and its CB/DB writes were flushed by its
         * release; the next arena of the same pass may now begin. */
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_BATCH_COMPLETED serial=%llu arena=%u arenas=%u",
            (unsigned long long)j->serial,j->launched-1u,j->chain.count);
        return launch_batch(j,j->launched);
    }
    if(value==j->serial) {
        if(j->ring_address) {
            if(ps5vk_tess_ring_completed(&j->ring))retain("ring-completion-state");
            int rc=ps5vk_tess_ring_restore(&j->ring);
            ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_QUEUE_RING_RESTORED serial=%llu rc=%d state=%u",
                (unsigned long long)j->serial,rc,(unsigned)j->ring.state);
            if(rc)retain("ring-restore-unresolved");
            if(ps5vk_hs_completed(&j->offchip))retain("offchip-completion-state");
            int hs_rc=ps5vk_hs_restore(&j->offchip);
            ps5log_printf(PS5LOG_MARK,"PS5VK_TESS_OFFCHIP_RESTORED serial=%llu rc=%d state=%u",
                (unsigned long long)j->serial,hs_rc,(unsigned)j->offchip.state);
            if(hs_rc)retain("offchip-restore-unresolved");
#if defined(PS5VK_TESS_HULL_TRACE) && PS5VK_TESS_HULL_TRACE
            if(j->hull_trace && j->serial==17) {
                cache(j->hull_trace,8192u);
                for(unsigned slot=0;slot<128;++slot) {
                    const uint32_t *w=j->hull_trace+slot*16u;
                    if(w[3])ps5log_printf(PS5LOG_MARK,
                        "PS5VK_TESS_HULL_TRACE serial=17 slot=%u s=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x v=%08x,%08x,%08x,%08x",
                        slot,w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],
                        w[8],w[9],w[10],w[11]);
                }
            }
#endif
            /* Offchip is GPU scratch, not a host readback. Completion and
             * checked lease restoration above establish retirement; scanning
             * its entire contents here was a private investigation aid, not
             * a visibility dependency or an application result. */
            __atomic_store_n(&ring_owner,NULL,__ATOMIC_RELEASE);
        }
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE && PS5VK_GRAPHICS_SCISSOR_PROBE!=15
        volatile uint32_t *probe=(volatile uint32_t *)(label+1);
        cache((const void *)probe,PS5VK_GRAPHICS_PROBE_REGISTERS*4);
        for(unsigned i=0;i<PS5VK_GRAPHICS_PROBE_REGISTERS;++i)
            ps5log_printf(PS5LOG_MARK,"PS5VK_GPU_REGISTER serial=%llu register=%x value=%08x sentinel=%u",
                (unsigned long long)j->serial,ps5vk_graphics_probe_registers[i],probe[i],probe[i]==0xd15ea5e0u+i);
#endif
        void *address=NULL;VkDeviceSize bytes=0;
        if(j->color) {
            if(ps5vk_image_span(d,j->color,&address,&bytes)!=VK_SUCCESS)return VK_ERROR_DEVICE_LOST;
            cache(address,(size_t)bytes);
        }
        if(j->readback_buffer) {
            void *destination;VkDeviceSize destination_bytes;
            VkImage image=j->readback_image;
            if(!image || ps5vk_buffer_span(d,j->readback_buffer,0,VK_WHOLE_SIZE,
                    &destination,&destination_bytes)!=VK_SUCCESS ||
               ps5vk_readback_detile(image,(size_t)j->readback_stride,
                    destination,(size_t)destination_bytes,address,(size_t)bytes))
                return VK_ERROR_DEVICE_LOST;
            /* This bounded implementation performs the transfer-copy result
             * publication on the CPU only after exact GPU completion. The
             * allocation remains non-coherent; the application still calls
             * vkInvalidateMappedMemoryRanges before host reads. */
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_GRAPHICS_READBACK serial=%llu width=%u height=%u bytes=%llu mode=cpu-detile-after-gpu",
                (unsigned long long)j->serial,image->info.extent.width,image->info.extent.height,
                (unsigned long long)destination_bytes);
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 4
            /* Private diagnostic only. Never change pixels or upstream's
             * comparator; count just the exact single-layer packed image. */
            if((image->info.format==VK_FORMAT_R8G8B8A8_UNORM ||
                image->info.format==VK_FORMAT_B8G8R8A8_UNORM) &&
               image->info.arrayLayers==1 && image->info.extent.depth==1 &&
               destination_bytes==(uint64_t)image->info.extent.width*image->info.extent.height*4u) {
                struct ps5vk_readback_content content;
                if(!ps5vk_readback_content(destination,(size_t)destination_bytes,&content))
                    ps5log_printf(PS5LOG_MARK,
                        "PS5VK_READBACK_CONTENT serial=%llu pixels=%llu nonblack=%llu opaque=%llu gray128=%llu hash=%08x",
                        (unsigned long long)j->serial,(unsigned long long)content.pixels,
                        (unsigned long long)content.nonblack,(unsigned long long)content.opaque,
                        (unsigned long long)content.gray128,content.hash);
            }
#endif
        }
        if(ps5vk_layout_commit(&j->layouts)!=VK_SUCCESS)return VK_ERROR_DEVICE_LOST;
        j->complete=1;*completed=j->serial;
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_COMPLETED serial=%llu image_bytes=%llu",(unsigned long long)j->serial,(unsigned long long)bytes);
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
        /* The occlusion readout is emitted only AFTER the completion marker, so
         * the evidence itself shows the slot was read after the producing
         * submission retired rather than merely stating that it was. The
         * memory is non-coherent: invalidate before reading. */
        if(j->slot_active && j->slot.address) {
            cache((const void *)j->slot.address,(size_t)PS5VK_OCCLUSION_PROBE_BYTES);
            const volatile uint64_t *slot=(const volatile uint64_t *)j->slot.address;
            const uint64_t availability=UINT64_C(1)<<63;
            uint64_t counter=0;
            unsigned available=0,first_pair=0,highest_pair=0;
            uint32_t mask_lo=0,mask_hi=0;
            for(unsigned i=0;i<PS5VK_OCCLUSION_PROBE_PAIRS;++i) {
                uint64_t begin=slot[2*i],end=slot[2*i+1];
                int ok=(begin&availability)&&(end&availability);
                if(!ok)continue;
                uint64_t delta=(end&~availability)-(begin&~availability);
                if(!available)first_pair=i;
                highest_pair=i;
                ++available;
                if(i<32)mask_lo|=UINT32_C(1)<<i;else mask_hi|=UINT32_C(1)<<(i-32);
                counter+=delta;
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_OCCLUSION_PROBE_PAIR serial=%llu index=%u begin=%016llx end=%016llx delta=%llu",
                    (unsigned long long)j->serial,i,(unsigned long long)begin,
                    (unsigned long long)end,(unsigned long long)delta);
            }
            /* Report only measured facts. No self-certifying validity flag: the
             * verifier derives validity from these numbers and the invariants
             * it checks itself (contiguous unique indices from first_pair,
             * highest_pair matching the last index, deltas summing to the
             * counter, availability bit set in both words). */
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_OCCLUSION_PROBE_SLOT serial=%llu pairs=%u available=%u first_pair=%u highest_pair=%u mask_lo=%08x mask_hi=%08x counter=%llu",
                (unsigned long long)j->serial,(unsigned)PS5VK_OCCLUSION_PROBE_PAIRS,
                available,first_pair,highest_pair,mask_lo,mask_hi,
                (unsigned long long)counter);
        }
#endif
        return VK_SUCCESS;
    }
    if(value || now(NULL)-j->start>UINT64_C(3000000000))return VK_ERROR_DEVICE_LOST;
    return VK_SUCCESS;
}
void ps5vk_native_graphics_queue_configure(VkDevice d)
{
    d->submit_backend=(struct ps5vk_queue_backend){prepare,launch,poll,release};
    d->progress=(struct ps5vk_progress){NULL,ps5vk_queue_poll,now,pause_wait};
    d->graphics_submit_enabled=VK_TRUE;
}

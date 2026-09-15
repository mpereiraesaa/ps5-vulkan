#include "vk_queue.h"
#include "vk_indirect.h"
#include "draw_prepare_ps5.h"
#include "command_arena_ps5.h"
#include "graphics_sync.h"
#include "image_layout_state.h"
#include "texture_copy.h"
#include "texture_dma.h"
#include "upload_commands_ps5.h"
#include "readback_commands_ps5.h"
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
    struct ps5vk_command_arena commands;
    struct ps5vk_command_arena slot;
    struct ps5vk_prepared_draw draws[PS5VK_MAX_OPERATIONS];
    unsigned count, words, attempted, complete, slot_active;
    uint64_t serial, start;
    VkImage color;
    VkImage readback_image;
    VkBuffer readback_buffer;
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
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    if(j->slot_active && ps5vk_command_arena_release(&j->slot)!=VK_SUCCESS)
        retain("occlusion-slot-release");
#endif
    if(ps5vk_command_arena_release(&j->commands)!=VK_SUCCESS)retain("command-release");
    for(unsigned i=0;i<j->count;++i)ps5vk_native_release_draw(&j->draws[i]);
    free(j);
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
    VkResult rc=ps5vk_command_arena_create(&j->commands);
    if(rc==VK_ERROR_DEVICE_LOST)retain("upload-command-create");
    if(rc!=VK_SUCCESS)goto fail;
    uint32_t *start=j->commands.address,*cursor=start,*end=start+PS5VK_COMMAND_ARENA_WORDS;
    size_t n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
    if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
    unsigned readback=0;
    for(unsigned i=0;i<count;++i)
        readback|=cb->operations[first+i].type==PS5VK_COPY_IMAGE_BUFFER;
    if(readback) {
        struct ps5vk_readback_plan plan={0};
        rc=ps5vk_readback_commands(d,cb->operations+first,count,NULL,&j->layouts,&plan);
        if(rc==VK_SUCCESS) {
            j->color=j->readback_image=plan.image;j->readback_buffer=plan.buffer;
        }
    } else rc=ps5vk_upload_commands(d,cb->operations+first,count,NULL,&j->layouts,&cursor,end,cache);
    if(rc!=VK_SUCCESS)goto fail;
    n=ps5vk_graphics_release(cursor,(size_t)(end-cursor),
        (uintptr_t)ps5vk_command_arena_label(&j->commands),j->serial);
    if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
    j->words=(unsigned)(cursor-start);*out=j;
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
           type!=PS5VK_CLEAR_DEPTH_STENCIL_IMAGE)
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
    /* This backend executes ONE subpass. A render pass that declares more is
     * refused here as well as at submission: subpass transitions, their
     * attachment lifetime and their ordering are unimplemented, and running
     * only the first subpass of a pass that declares two would silently drop
     * half the work. */
    if(pass->subpass_count!=1)return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct ps5vk_subpass *subpass=ps5vk_render_pass_subpass(pass,0);
    int depth=subpass->depth.attachment!=VK_ATTACHMENT_UNUSED;
    VkFormat color_format=begin->framebuffer->attachments[0]->image->info.format;
    if(pass->attachment_count!=(depth?2u:1u) || pass->dependency_count ||
        (depth && subpass->depth.attachment!=1) || subpass->color.attachment!=0 ||
        (color_format!=VK_FORMAT_B8G8R8A8_UNORM && color_format!=VK_FORMAT_R8G8B8A8_UNORM))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_attachment_plan color_plan={0},depth_plan={0};
    if(ps5vk_attachment_plan(&pass->attachments[0],color_format,
        subpass->color.layout,VK_FALSE,&color_plan)!=VK_SUCCESS)return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t clear_word=0;
    if(color_plan.clear) {
        VkImage image=begin->framebuffer->attachments[0]->image;
        int clear_ok=color_format==VK_FORMAT_B8G8R8A8_UNORM ?
            ps5vk_color_clear_bgra8(begin->clears[0].color.float32,&clear_word) :
            ps5vk_color_clear_rgba8(begin->clears[0].color.float32,&clear_word);
        if(image->info.format!=color_format || begin->clear_count<1 || !clear_ok ||
           begin->render_area.offset.x || begin->render_area.offset.y ||
           begin->render_area.extent.width!=image->info.extent.width ||
           begin->render_area.extent.height!=image->info.extent.height)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(depth) {
        const VkAttachmentDescription *a=&pass->attachments[1];
        VkImage image=begin->framebuffer->attachments[1]->image;
        if(ps5vk_attachment_plan(a,VK_FORMAT_D32_SFLOAT,subpass->depth.layout,
            VK_TRUE,&depth_plan)!=VK_SUCCESS ||
            image->info.extent.width!=begin->framebuffer->width ||
            image->info.extent.height!=begin->framebuffer->height)return VK_ERROR_FEATURE_NOT_PRESENT;
        if(depth_plan.clear) {
            float clear=begin->clears[1].depthStencil.depth;
            if(begin->clear_count<2 || !(clear>=0 && clear<=1) ||
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
    j->serial=s->serial; j->color=begin->framebuffer->attachments[0]->image;
    phase="command-arena";
    VkResult rc=ps5vk_command_arena_create(&j->commands);
    if(rc==VK_ERROR_DEVICE_LOST)retain("command-create");
    if(rc!=VK_SUCCESS)goto fail;
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
    /* Record the scoped render-pass transitions transactionally. Resource
     * state becomes committed only after the exact GPU completion label. */
    phase="attachment-layout";
    rc=ps5vk_layout_transition(&j->layouts,j->color,
        pass->attachments[0].initialLayout,pass->attachments[0].finalLayout);
    if(rc!=VK_SUCCESS)goto fail;
    if(depth) {
        rc=ps5vk_layout_transition(&j->layouts,begin->framebuffer->attachments[1]->image,
            pass->attachments[1].initialLayout,pass->attachments[1].finalLayout);
        if(rc!=VK_SUCCESS)goto fail;
    }
    ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT];
    if(ps5_color_select_runtime_defaults(defaults,sceAgcGetRegisterDefaults())) {rc=VK_ERROR_INITIALIZATION_FAILED;goto fail;}
    uint32_t *start=j->commands.address,*cursor=start,*end=start+PS5VK_COMMAND_ARENA_WORDS;
    cursor+=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
    if(color_plan.clear) {
        void *address;VkDeviceSize bytes;
        rc=ps5vk_image_span(d,j->color,&address,&bytes);
        if(rc!=VK_SUCCESS)goto fail;
        cache(address,(size_t)bytes);
        size_t n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)address,bytes,clear_word);
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,"PS5VK_COLOR_CLEAR_PREPARED serial=%llu bgra=%08x bytes=%llu",
            (unsigned long long)j->serial,clear_word,(unsigned long long)bytes);
    }
    if(depth && depth_plan.clear) {
        void *address;VkDeviceSize bytes;
        rc=ps5vk_image_span(d,begin->framebuffer->attachments[1]->image,&address,&bytes);
        if(rc!=VK_SUCCESS)goto fail;
        uint32_t value;memcpy(&value,&begin->clears[1].depthStencil.depth,sizeof(value));
        cache(address,(size_t)bytes);
        size_t n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)address,bytes,value);
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
    }
    phase="prelude";
    rc=ps5vk_upload_commands(d,cb->operations+range_first,first-range_first,j->color,
        &j->layouts,&cursor,end,cache);
    if(rc!=VK_SUCCESS)goto fail;
    phase="draw";
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    if(j->slot_active) {
        size_t n=ps5vk_graphics_occlusion_event(cursor,(size_t)(end-cursor),(uint64_t)(uintptr_t)j->slot.address);
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_OCCLUSION_PROBE_BEGIN serial=%llu base=%llx pairs=%u",
            (unsigned long long)j->serial,(unsigned long long)(uintptr_t)j->slot.address,
            (unsigned)PS5VK_OCCLUSION_PROBE_PAIRS);
    }
#endif
    for(unsigned i=0;i<body_count;++i) {
        const struct ps5vk_operation *recorded=body[i];
        struct ps5vk_operation resolved;
        const struct ps5vk_operation *op=recorded;
        if(ps5vk_indirect_graphics_operation(recorded->type)) {
            rc=ps5vk_indirect_resolve(d,recorded,&resolved);if(rc!=VK_SUCCESS)goto fail;
            op=&resolved;
        }
        struct ps5vk_prepared_draw *draw=&j->draws[j->count];
        struct ps5vk_native_graphics_pipeline *p=op->pipeline->graphics_state;
        if(!p || !p->pair || !p->pair->ready){rc=VK_ERROR_UNKNOWN;goto fail;}
        /* Check each sampled resource, not just element zero of set zero.
         * Preparation validates generations and copies exactly these tables. */
        if(op->pipeline->set_count>PS5VK_MAX_SETS){rc=VK_ERROR_UNKNOWN;goto fail;}
        for(unsigned set_index=0;set_index<op->pipeline->set_count;++set_index) {
            if(p->pair->runtime_arguments.enabled &&
               !p->pair->runtime_arguments.fragment_descriptor_valid[set_index] &&
               !p->pair->runtime_arguments.vertex_descriptor_valid[set_index])continue;
            VkDescriptorSet set=op->sets[set_index];
            if(!set || !set->pool || set->pool->device!=d ||
               set->generation!=op->generations[set_index] ||
               memcmp(&set->signature,&op->pipeline->sets[set_index],sizeof(set->signature))) {
                rc=VK_ERROR_UNKNOWN;goto fail;
            }
            for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
                const struct ps5vk_binding *binding=&set->signature.binding[b];
                const VkDescriptorType type=set->signature.type[b];
                const int buffer_type=type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                    type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                if(binding->count && type!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                   !buffer_type) {
                    rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
                }
                if(binding->first>PS5VK_MAX_DESCRIPTORS ||
                   binding->count>PS5VK_MAX_DESCRIPTORS-binding->first){rc=VK_ERROR_UNKNOWN;goto fail;}
                /* Only the bindings the compiled stages dereference are a
                 * requirement; the table is shared, so the two stage masks are
                 * ORed. A zero mask keeps the whole declaration (fail closed). */
                if(p->pair->runtime_arguments.enabled) {
                    const uint64_t used=
                        p->pair->runtime_arguments.vertex_used_bindings[set_index]|
                        p->pair->runtime_arguments.fragment_used_bindings[set_index];
                    if(used && binding->count && !(used&(UINT64_C(1)<<b)))continue;
                }
                for(unsigned e=0;e<binding->count;++e) {
                    unsigned index=binding->first+e;
                    if(!set->defined[index]){rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
                    if(buffer_type) {
                        /* The encoder resolves and validates the base address;
                         * a null handle must never be encoded. */
                        if(!set->buffers[index].buffer) {
                            rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
                        }
                        continue;
                    }
                    if(!set->image_resources[index] ||
                       set->images[index].imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                        rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
                    }
                    rc=ps5vk_layout_require(&j->layouts,set->image_resources[index],set->images[index].imageLayout);
                    if(rc!=VK_SUCCESS)goto fail;
                }
            }
        }
        struct ps5vk_index_fetch indices={0};
        if(op->type==PS5VK_DRAW_INDEXED) {
            if(!op->pipeline->vertex_binding_count){rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
            rc=ps5vk_index_fetch_prepare(d,op,&indices);if(rc!=VK_SUCCESS)goto fail;
        }
        const uint32_t vertex_usage=p->pair->runtime_arguments.enabled?
            p->pair->runtime_arguments.vertex_buffer_usage_mask:
            (op->pipeline->vertex_binding_count?1u:0u);
        if(vertex_usage) {
            const struct ps5vk_graphics_key key={.vertex_binding_count=op->pipeline->vertex_binding_count,
                .vertex_attribute_count=op->pipeline->vertex_attribute_count,
                .vertex_bindings=op->pipeline->vertex_bindings,.vertex_attributes=op->pipeline->vertex_attributes};
            rc=ps5vk_native_prepare_vertex_draw_masked(d,op,&begin->render_area,defaults,&key,
                (uintptr_t)p->pair,vertex_usage,draw);
        } else rc=ps5vk_native_prepare_resource_draw(d,op,&begin->render_area,defaults,(uintptr_t)p->pair,draw);
        if(rc!=VK_SUCCESS)goto fail;
        ++j->count;
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
        if(!p->global_table){rc=VK_ERROR_UNKNOWN;goto fail;}
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE && PS5VK_GRAPHICS_SCISSOR_PROBE!=15
        if(draw->texture_table)
            for(unsigned k=0;k<12;++k)
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TEXTURE_DESCRIPTOR serial=%llu draw=%u word=%u value=%08x",
                    (unsigned long long)j->serial,j->count-1,k,draw->texture_table[k]);
#endif
        if(p->pair->runtime_arguments.enabled) {
            uint32_t tables[PS5VK_RUNTIME_DESCRIPTOR_SETS]={0};
            for(unsigned s=0;s<PS5VK_RUNTIME_DESCRIPTOR_SETS;++s)
                tables[s]=(uint32_t)(uintptr_t)draw->descriptor_tables[s];
            rc=ps5vk_native_emit_runtime_draw(&cursor,(uint32_t)(end-cursor),draw->state,
                draw->state,draw->bytes,op,(uint32_t)(uintptr_t)draw->vertex_table,tables,
                op->type==PS5VK_DRAW_INDEXED?&indices:NULL,sceAgcDcbDrawIndex);
        } else if(vertex_usage) {
            if(!draw->vertex_table){rc=VK_ERROR_UNKNOWN;goto fail;}
            if(op->pipeline->set_count) {
                if(!draw->texture_table){rc=VK_ERROR_UNKNOWN;goto fail;}
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
        if(rc!=VK_SUCCESS)goto fail;
    }
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    if(j->slot_active) {
        size_t n=ps5vk_graphics_occlusion_event(cursor,(size_t)(end-cursor),
            (uint64_t)(uintptr_t)j->slot.address+8);
        if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_OCCLUSION_PROBE_END serial=%llu base_plus_8=%llx",
            (unsigned long long)j->serial,(unsigned long long)(uintptr_t)j->slot.address+8);
    }
#endif
    phase="postlude";
    if(last+1<range_end) {
        struct ps5vk_readback_plan plan={0};
        rc=ps5vk_readback_commands(d,cb->operations+last+1,range_end-last-1,j->color,&j->layouts,&plan);
        if(rc!=VK_SUCCESS)goto fail;
        j->readback_image=plan.image;j->readback_buffer=plan.buffer;
    }
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE && PS5VK_GRAPHICS_SCISSOR_PROBE!=15
    _Static_assert(8+PS5VK_GRAPHICS_PROBE_REGISTERS*4<=64,"probe must not overlap command words or leave reserved tail");
    uint32_t *probe=(uint32_t *)(ps5vk_command_arena_label(&j->commands)+1);
    for(unsigned i=0;i<PS5VK_GRAPHICS_PROBE_REGISTERS;++i)probe[i]=0xd15ea5e0u+i;
    size_t probe_words=ps5vk_graphics_register_probe(cursor,(size_t)(end-cursor),(uintptr_t)probe);
    if(!probe_words){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=probe_words;
#endif
    phase="release-packet";
    size_t n=ps5vk_graphics_release(cursor,(size_t)(end-cursor),(uintptr_t)ps5vk_command_arena_label(&j->commands),j->serial);
    if(!n){rc=VK_ERROR_UNKNOWN;goto fail;} cursor+=n; j->words=(unsigned)(cursor-start);
    *out=j;
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_PREPARED serial=%llu draws=%u words=%u",(unsigned long long)j->serial,j->count,j->words);
    return VK_SUCCESS;
fail:
    ps5log_printf(PS5LOG_ERR,"PS5VK_GRAPHICS_PREPARE_FAILED serial=%llu phase=%s rc=%d",
        (unsigned long long)j->serial,phase,rc);
    release(d,j);return rc;
}
static VkResult launch(VkDevice d,void *opaque)
{
    (void)d;struct graphics_job *j=opaque;
    cache(j->commands.address,PS5VK_COMMAND_ARENA_BYTES);
    struct ps5_agc_submit packet={j->commands.address,j->words,0,{0,0,0}};
    j->start=now(NULL); if(!j->start)return VK_ERROR_DEVICE_LOST;
    j->attempted=1;
    struct ps5vk_submit_result result=ps5vk_submit_suspend(&packet);
    int rc=result.submit_rc;
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_SUBMIT serial=%llu rc=%d",(unsigned long long)j->serial,rc);
    if (result.suspend_attempted) {
        rc=result.suspend_rc;
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_SUSPEND_POINT serial=%llu rc=%d",(unsigned long long)j->serial,rc);
    }
    return rc ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VkResult poll(VkDevice d,void *opaque,uint64_t *completed)
{
    struct graphics_job *j=opaque;*completed=0;
    volatile uint64_t *label=ps5vk_command_arena_label(&j->commands);
    cache((const void *)label,8);uint64_t value=__atomic_load_n(label,__ATOMIC_ACQUIRE);
    if(value==j->serial) {
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
               ps5vk_rgba8_64k_rx_detile(destination,(size_t)destination_bytes,address,(size_t)bytes,
                    image->info.extent.width,image->info.extent.height))
                return VK_ERROR_DEVICE_LOST;
            /* This bounded implementation performs the transfer-copy result
             * publication on the CPU only after exact GPU completion. The
             * allocation remains non-coherent; the application still calls
             * vkInvalidateMappedMemoryRanges before host reads. */
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_GRAPHICS_READBACK serial=%llu width=%u height=%u bytes=%llu mode=cpu-detile-after-gpu",
                (unsigned long long)j->serial,image->info.extent.width,image->info.extent.height,
                (unsigned long long)destination_bytes);
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

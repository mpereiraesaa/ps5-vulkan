#include "vk_queue.h"
#include "vk_indirect.h"
#include "draw_prepare_ps5.h"
#include "command_arena_ps5.h"
#include "graphics_sync.h"
#include "image_layout_state.h"
#include "texture_copy.h"
#include "texture_dma.h"
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
struct graphics_job {
    struct ps5vk_command_arena commands;
    struct ps5vk_prepared_draw draws[PS5VK_MAX_OPERATIONS];
    unsigned count, words, attempted, complete;
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
    if(ps5vk_command_arena_release(&j->commands)!=VK_SUCCESS)retain("command-release");
    for(unsigned i=0;i<j->count;++i)ps5vk_native_release_draw(&j->draws[i]);
    free(j);
}
static VkResult prepare(VkDevice d,const struct ps5vk_submission *s,void **out)
{
    const char *phase="shape";
    *out=NULL;
    /* One color pass, optional D32 and texture-upload prelude.  LOAD preserves
     * an attachment only when its tracked initial layout matches.  CLEAR is
     * bounded to the full render area until a rectangular clear path exists. */
    if(s->count!=1 || !s->serial)return VK_ERROR_FEATURE_NOT_PRESENT;
    VkCommandBuffer cb=s->buffers[0];
    uint32_t range_first=ps5vk_submission_first_operation(s,0);
    uint32_t range_count=ps5vk_submission_operation_count(s,0);
    if(range_first>cb->operation_count || range_count>cb->operation_count-range_first)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t range_end=range_first+range_count;
    unsigned first=range_first;
    while(first<range_end && cb->operations[first].type!=PS5VK_BEGIN_RENDER_PASS) {
        unsigned type=cb->operations[first].type;
        if(type!=PS5VK_BARRIER && type!=PS5VK_IMAGE_BARRIER && type!=PS5VK_COPY_BUFFER_IMAGE)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        ++first;
    }
    unsigned last=first+1;
    while(last<range_end && cb->operations[last].type!=PS5VK_END_RENDER_PASS)++last;
    if(first>=range_end || last>=range_end || last<first+2)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct ps5vk_operation *begin=&cb->operations[first]; VkRenderPass pass=begin->render_pass;
    int depth=pass->depth.attachment!=VK_ATTACHMENT_UNUSED;
    VkFormat color_format=begin->framebuffer->attachments[0]->image->info.format;
    if(pass->attachment_count!=(depth?2u:1u) || pass->dependency_count ||
        (depth && pass->depth.attachment!=1) || pass->color.attachment!=0 ||
        (color_format!=VK_FORMAT_B8G8R8A8_UNORM && color_format!=VK_FORMAT_R8G8B8A8_UNORM))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_attachment_plan color_plan={0},depth_plan={0};
    if(ps5vk_attachment_plan(&pass->attachments[0],color_format,
        pass->color.layout,VK_FALSE,&color_plan)!=VK_SUCCESS)return VK_ERROR_FEATURE_NOT_PRESENT;
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
        if(ps5vk_attachment_plan(a,VK_FORMAT_D32_SFLOAT,pass->depth.layout,
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
    for(unsigned i=first+1;i<last;++i)
        if(cb->operations[i].type!=PS5VK_DRAW &&
           cb->operations[i].type!=PS5VK_DRAW_INDEXED &&
           !ps5vk_indirect_graphics_operation(cb->operations[i].type))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    struct graphics_job *j=calloc(1,sizeof(*j)); if(!j)return VK_ERROR_OUT_OF_HOST_MEMORY;
    j->serial=s->serial; j->color=begin->framebuffer->attachments[0]->image;
    phase="command-arena";
    VkResult rc=ps5vk_command_arena_create(&j->commands);
    if(rc==VK_ERROR_DEVICE_LOST)retain("command-create");
    if(rc!=VK_SUCCESS)goto fail;
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
    for(unsigned i=range_first;i<first;++i) {
        const struct ps5vk_operation *op=&cb->operations[i];
        if(op->type==PS5VK_BARRIER) {
            if(op->buffer_barrier.buffer || op->src_stage!=VK_PIPELINE_STAGE_HOST_BIT ||
               op->dst_stage!=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT ||
               op->src_access!=VK_ACCESS_HOST_WRITE_BIT ||
               op->dst_access!=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
            }
        } else if(op->type==PS5VK_IMAGE_BARRIER) {
            const VkImageMemoryBarrier *b=&op->image_barrier;
            /* Scoped upload profiles plus the original CTS triangle's discard
             * -> color-attachment transition. */
            if(!((b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT) ||
                (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && b->dstAccessMask==VK_ACCESS_SHADER_READ_BIT) ||
                (b->image==j->color && b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
                b->newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && !b->srcAccessMask &&
                b->dstAccessMask==(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)))) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
            }
            rc=ps5vk_layout_transition(&j->layouts,op->image_barrier.image,
                op->image_barrier.oldLayout,op->image_barrier.newLayout);
            if(rc!=VK_SUCCESS)goto fail;
            size_t n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
            if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        } else if(op->type==PS5VK_COPY_BUFFER_IMAGE) {
            if(op->copy_layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
            void *source,*destination;VkDeviceSize source_bytes,destination_bytes;
            struct ps5vk_texture_copy copy;
            rc=ps5vk_layout_require(&j->layouts,op->copy_image,op->copy_layout);
            if(rc!=VK_SUCCESS)goto fail;
            rc=ps5vk_buffer_span(d,op->copy_source,0,VK_WHOLE_SIZE,&source,&source_bytes);
            if(rc!=VK_SUCCESS)goto fail;
            rc=ps5vk_image_span(d,op->copy_image,&destination,&destination_bytes);
            if(rc!=VK_SUCCESS)goto fail;
            rc=ps5vk_texture_copy_plan_for_image(op->copy_image,
                source_bytes,destination_bytes,&op->copy_region,&copy);
            if(rc!=VK_SUCCESS)goto fail;
            cache(source,(size_t)source_bytes);
            size_t n=ps5vk_texture_dma(cursor,(size_t)(end-cursor),(uintptr_t)source,(uintptr_t)destination,&copy);
            if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        } else {rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
    }
    phase="draw";
    for(unsigned i=first+1;i<last;++i) {
        const struct ps5vk_operation *recorded=&cb->operations[i];
        struct ps5vk_operation resolved;
        const struct ps5vk_operation *op=recorded;
        if(ps5vk_indirect_graphics_operation(recorded->type)) {
            rc=ps5vk_indirect_resolve(d,recorded,&resolved);if(rc!=VK_SUCCESS)goto fail;
            op=&resolved;
        }
        /* Require the uploaded image's predicted shader-readable layout. */
        if(op->pipeline->set_count) {
            if(!op->sets[0] || !op->sets[0]->defined[0]){rc=VK_ERROR_UNKNOWN;goto fail;}
            if(op->sets[0]->images[0].imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
            }
            rc=ps5vk_layout_require(&j->layouts,op->sets[0]->image_resources[0],op->sets[0]->images[0].imageLayout);
            if(rc!=VK_SUCCESS)goto fail;
        }
        struct ps5vk_prepared_draw *draw=&j->draws[j->count];
        struct ps5vk_native_graphics_pipeline *p=op->pipeline->graphics_state;
        if(!p || !p->pair || !p->pair->ready){rc=VK_ERROR_UNKNOWN;goto fail;}
        struct ps5vk_index_fetch indices={0};
        if(op->type==PS5VK_DRAW_INDEXED) {
            if(!op->pipeline->vertex_binding_count){rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
            rc=ps5vk_index_fetch_prepare(d,op,&indices);if(rc!=VK_SUCCESS)goto fail;
        }
        if(op->pipeline->vertex_binding_count) {
            const struct ps5vk_graphics_key key={.vertex_binding_count=op->pipeline->vertex_binding_count,
                .vertex_attribute_count=op->pipeline->vertex_attribute_count,
                .vertex_bindings=&op->pipeline->vertex_binding,.vertex_attributes=op->pipeline->vertex_attributes};
            rc=ps5vk_native_prepare_vertex_draw(d,op,&begin->render_area,defaults,&key,(uintptr_t)p->pair,draw);
        } else rc=ps5vk_native_prepare_draw(d,op,&begin->render_area,defaults,draw);
        if(rc!=VK_SUCCESS)goto fail;
        ++j->count;
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE
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
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE
        if(draw->texture_table)
            for(unsigned k=0;k<12;++k)
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_TEXTURE_DESCRIPTOR serial=%llu draw=%u word=%u value=%08x",
                    (unsigned long long)j->serial,j->count-1,k,draw->texture_table[k]);
#endif
        if(op->pipeline->vertex_binding_count) {
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
    phase="postlude";
    if(last+1<range_end) {
        if(range_end!=last+5 ||
           cb->operations[last+1].type!=PS5VK_IMAGE_BARRIER ||
           cb->operations[last+2].type!=PS5VK_COPY_IMAGE_BUFFER ||
           cb->operations[last+3].type!=PS5VK_BARRIER ||
           cb->operations[last+4].type!=PS5VK_BARRIER) {
            rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
        }
        const struct ps5vk_operation *transition=&cb->operations[last+1];
        const struct ps5vk_operation *copy=&cb->operations[last+2];
        const struct ps5vk_operation *host=&cb->operations[last+3];
        const struct ps5vk_operation *aggregate=&cb->operations[last+4];
        const VkImageMemoryBarrier *b=&transition->image_barrier;
        if(b->image!=j->color || b->oldLayout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
           b->newLayout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           b->srcAccessMask!=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT ||
           b->dstAccessMask!=VK_ACCESS_TRANSFER_READ_BIT ||
           (transition->src_stage!=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
            transition->src_stage!=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) ||
           transition->dst_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT ||
           copy->copy_image!=j->color || copy->copy_layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           !copy->copy_destination || host->buffer_barrier.buffer!=copy->copy_destination ||
           host->src_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT || host->dst_stage!=VK_PIPELINE_STAGE_HOST_BIT ||
           host->src_access!=VK_ACCESS_TRANSFER_WRITE_BIT || host->dst_access!=VK_ACCESS_HOST_READ_BIT ||
           aggregate->buffer_barrier.buffer || aggregate->src_access || aggregate->dst_access ||
           aggregate->src_stage!=VK_PIPELINE_STAGE_TRANSFER_BIT ||
           aggregate->dst_stage!=VK_PIPELINE_STAGE_HOST_BIT) {
            rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
        }
        rc=ps5vk_layout_transition(&j->layouts,j->color,b->oldLayout,b->newLayout);
        if(rc!=VK_SUCCESS)goto fail;
        j->readback_image=j->color;j->readback_buffer=copy->copy_destination;
    }
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE
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
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE
        volatile uint32_t *probe=(volatile uint32_t *)(label+1);
        cache((const void *)probe,PS5VK_GRAPHICS_PROBE_REGISTERS*4);
        for(unsigned i=0;i<PS5VK_GRAPHICS_PROBE_REGISTERS;++i)
            ps5log_printf(PS5LOG_MARK,"PS5VK_GPU_REGISTER serial=%llu register=%x value=%08x sentinel=%u",
                (unsigned long long)j->serial,ps5vk_graphics_probe_registers[i],probe[i],probe[i]==0xd15ea5e0u+i);
#endif
        void *address;VkDeviceSize bytes;
        if(ps5vk_image_span(d,j->color,&address,&bytes)!=VK_SUCCESS)return VK_ERROR_DEVICE_LOST;
        cache(address,(size_t)bytes);
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

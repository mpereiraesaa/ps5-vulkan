#include "vk_queue.h"
#include "draw_prepare_ps5.h"
#include "command_arena_ps5.h"
#include "graphics_sync.h"
#include "image_layout_state.h"
#include "texture_copy.h"
#include "texture_dma.h"
#include "color_clear.h"
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
    *out=NULL;
    /* One color pass, optional full-frame D32 clear and texture-upload prelude.
     * Full-target color CLEAR and DONT_CARE are supported; LOAD preservation
     * and partial clears are not. Reject unsupported semantics rather than
     * silently skipping their records. */
    if(s->count!=1 || !s->serial)return VK_ERROR_FEATURE_NOT_PRESENT;
    VkCommandBuffer cb=s->buffers[0];
    unsigned first=0;
    while(first<cb->operation_count && (cb->operations[first].type==PS5VK_IMAGE_BARRIER ||
        cb->operations[first].type==PS5VK_COPY_BUFFER_IMAGE))++first;
    if(cb->operation_count<first+3 || cb->operations[first].type!=PS5VK_BEGIN_RENDER_PASS ||
        cb->operations[cb->operation_count-1].type!=PS5VK_END_RENDER_PASS)return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct ps5vk_operation *begin=&cb->operations[first]; VkRenderPass pass=begin->render_pass;
    int depth=pass->depth.attachment!=VK_ATTACHMENT_UNUSED;
    if(pass->attachment_count!=(depth?2u:1u) || pass->dependency_count ||
        (depth && pass->depth.attachment!=1) || pass->color.attachment!=0 ||
        pass->attachments[0].initialLayout!=VK_IMAGE_LAYOUT_UNDEFINED ||
        (pass->attachments[0].loadOp!=VK_ATTACHMENT_LOAD_OP_DONT_CARE &&
         pass->attachments[0].loadOp!=VK_ATTACHMENT_LOAD_OP_CLEAR) ||
        pass->attachments[0].finalLayout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)return VK_ERROR_FEATURE_NOT_PRESENT;
    int color_clear=pass->attachments[0].loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR;
    uint32_t clear_word=0;
    if(color_clear) {
        VkImage image=begin->framebuffer->attachments[0]->image;
        if(image->info.format!=VK_FORMAT_B8G8R8A8_UNORM || begin->clear_count<1 ||
           !ps5vk_color_clear_bgra8(begin->clears[0].color.float32,&clear_word) ||
           begin->render_area.offset.x || begin->render_area.offset.y ||
           begin->render_area.extent.width!=image->info.extent.width ||
           begin->render_area.extent.height!=image->info.extent.height)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(depth) {
        const VkAttachmentDescription *a=&pass->attachments[1];
        VkImage image=begin->framebuffer->attachments[1]->image;
        float clear=begin->clears[1].depthStencil.depth;
        if(a->format!=VK_FORMAT_D32_SFLOAT || a->initialLayout!=VK_IMAGE_LAYOUT_UNDEFINED ||
            a->loadOp!=VK_ATTACHMENT_LOAD_OP_CLEAR || a->finalLayout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
            begin->clear_count<2 || !(clear>=0 && clear<=1) ||
            begin->render_area.offset.x || begin->render_area.offset.y ||
            begin->render_area.extent.width!=begin->framebuffer->width ||
            begin->render_area.extent.height!=begin->framebuffer->height ||
            image->info.extent.width!=begin->framebuffer->width ||
            image->info.extent.height!=begin->framebuffer->height)return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    for(unsigned i=first+1;i+1<cb->operation_count;++i)
        if(cb->operations[i].type!=PS5VK_DRAW && cb->operations[i].type!=PS5VK_DRAW_INDEXED)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct graphics_job *j=calloc(1,sizeof(*j)); if(!j)return VK_ERROR_OUT_OF_HOST_MEMORY;
    j->serial=s->serial; j->color=begin->framebuffer->attachments[0]->image;
    VkResult rc=ps5vk_command_arena_create(&j->commands);
    if(rc==VK_ERROR_DEVICE_LOST)retain("command-create");
    if(rc!=VK_SUCCESS)goto fail;
    /* Record the scoped render-pass transitions transactionally. Resource
     * state becomes committed only after the exact GPU completion label. */
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
    if(color_clear) {
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
    if(depth) {
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
    for(unsigned i=0;i<first;++i) {
        const struct ps5vk_operation *op=&cb->operations[i];
        if(op->type==PS5VK_IMAGE_BARRIER) {
            const VkImageMemoryBarrier *b=&op->image_barrier;
            /* Scoped dependency profile: discard -> upload -> fragment read.
             * GENERAL/reverse transitions need additional hazard handling. */
            if(!((b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT) ||
                (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && b->dstAccessMask==VK_ACCESS_SHADER_READ_BIT))) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
            }
            rc=ps5vk_layout_transition(&j->layouts,op->image_barrier.image,
                op->image_barrier.oldLayout,op->image_barrier.newLayout);
            if(rc!=VK_SUCCESS)goto fail;
            size_t n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
            if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        } else {
            if(op->copy_layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
            void *source,*destination;VkDeviceSize source_bytes,destination_bytes;
            struct ps5vk_texture_copy copy;
            rc=ps5vk_layout_require(&j->layouts,op->copy_image,op->copy_layout);
            if(rc!=VK_SUCCESS)goto fail;
            rc=ps5vk_buffer_span(d,op->copy_source,0,VK_WHOLE_SIZE,&source,&source_bytes);
            if(rc!=VK_SUCCESS)goto fail;
            rc=ps5vk_image_span(d,op->copy_image,&destination,&destination_bytes);
            if(rc!=VK_SUCCESS)goto fail;
            rc=ps5vk_texture_copy_plan(op->copy_image->info.extent.width,op->copy_image->info.extent.height,
                source_bytes,destination_bytes,&op->copy_region,&copy);
            if(rc!=VK_SUCCESS)goto fail;
            cache(source,(size_t)source_bytes);
            size_t n=ps5vk_texture_dma(cursor,(size_t)(end-cursor),(uintptr_t)source,(uintptr_t)destination,&copy);
            if(!n){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=n;
        }
    }
    for(unsigned i=first+1;i+1<cb->operation_count;++i) {
        const struct ps5vk_operation *op=&cb->operations[i];
        /* Require the uploaded image's predicted shader-readable layout. */
        if(op->pipeline->set_count) {
            if(!op->set || !op->set->defined[0]){rc=VK_ERROR_UNKNOWN;goto fail;}
            if(op->set->images[0].imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
            }
            rc=ps5vk_layout_require(&j->layouts,op->set->image_resources[0],op->set->images[0].imageLayout);
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
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE
    _Static_assert(8+PS5VK_GRAPHICS_PROBE_REGISTERS*4<=64,"probe must not overlap command words or leave reserved tail");
    uint32_t *probe=(uint32_t *)(ps5vk_command_arena_label(&j->commands)+1);
    for(unsigned i=0;i<PS5VK_GRAPHICS_PROBE_REGISTERS;++i)probe[i]=0xd15ea5e0u+i;
    size_t probe_words=ps5vk_graphics_register_probe(cursor,(size_t)(end-cursor),(uintptr_t)probe);
    if(!probe_words){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=probe_words;
#endif
    size_t n=ps5vk_graphics_release(cursor,(size_t)(end-cursor),(uintptr_t)ps5vk_command_arena_label(&j->commands),j->serial);
    if(!n){rc=VK_ERROR_UNKNOWN;goto fail;} cursor+=n; j->words=(unsigned)(cursor-start);
    *out=j;
    ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_PREPARED serial=%llu draws=%u words=%u",(unsigned long long)j->serial,j->count,j->words);
    return VK_SUCCESS;
fail:
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

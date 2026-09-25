#include "vk_queue.h"
#include "vk_query_pool.h"
#include "color_attachment_contract.h"
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
#include "depth_layout.h"
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
#include "resolve_program.h"
#include "texture_descriptor.h"
enum { PS5VK_QUERY_COUNTER_PAIRS = 64, PS5VK_QUERY_SLOT_BYTES = 1024,
       PS5VK_QUERY_SLOTS = 64 };
#include "graphics_pipeline_ps5.h"
#include "runtime_graphics_compiler.h"
extern unsigned ps5vk_draw_prepare_site;
extern unsigned ps5vk_draw_state_site;
extern unsigned ps5vk_input_attachment_gate_site;
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
    struct ps5vk_command_arena query_arena;
    struct {
        VkQueryPool pool;
        uint32_t query;
        uint64_t *counters;
    } queries[PS5VK_QUERY_SLOTS];
    struct ps5vk_prepared_draw draws[PS5VK_MAX_OPERATIONS];
    /* The draws the DRIVER emits for its own resolve boundaries (DXVK262-T06).
     * They are prepared draws like any other, so their AGC context block has to
     * stay alive until the GPU has executed this submission: the command stream
     * only REFERENCES that block by address. The first version of the emission
     * released its prepared draw inside the walk, so the context the hardware
     * read had already been freed - the draw went out, ran, and wrote nothing. */
    struct ps5vk_prepared_draw resolve_draws[PS5VK_MAX_SUBPASSES];
    /* The loaded pair each of those draws was built from. The context block the
     * command stream references holds pointers into the uploaded shader code,
     * so the loaded state has to outlive the walk exactly like the prepared
     * draw itself. */
    void *resolve_states[PS5VK_MAX_SUBPASSES];
    unsigned count, words, attempted, complete, slot_active, launched, resolve_count;
    unsigned query_arena_active, query_count;
    /* One token per colour-to-texture barrier this submission emits. The token
     * names a private word of the arena that executes the wait, so the barrier
     * can block until the colour block has CONFIRMED its writeback: see
     * ps5vk_graphics_color_to_texture_wait in src/graphics_sync.c. */
    unsigned barrier_tokens;
    uint64_t serial, start;
    VkImage color;
    /* The targets this submission reads back after exact completion: one for
     * the draw module's own readback, and as many as the subpass named for the
     * render-pass module's. */
    struct ps5vk_readback_set readback;
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
    if(j->query_arena_active && ps5vk_command_arena_release(&j->query_arena)!=VK_SUCCESS)
        retain("query-arena-release");
    if(ps5vk_draw_batch_release(&j->chain)!=VK_SUCCESS)retain("command-release");
    for(unsigned i=0;i<j->count;++i)ps5vk_native_release_draw(&j->draws[i]);
    for(unsigned i=0;i<j->resolve_count;++i) {
        ps5vk_native_release_draw(&j->resolve_draws[i]);
        if(j->resolve_states[i])ps5vk_native_graphics_release(d,j->resolve_states[i]);
    }
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
    unsigned readback=0,readback_site=0;
    if(!count)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct graphics_job *j=calloc(1,sizeof(*j));
    if(!j)return VK_ERROR_OUT_OF_HOST_MEMORY;
    j->serial=s->serial;
    /* The chain opens its first arena with the acquire already emitted. */
    VkResult rc=ps5vk_draw_batch_open(&j->chain,j->serial);
    if(rc==VK_ERROR_DEVICE_LOST)retain("upload-command-create");
    if(rc!=VK_SUCCESS)goto fail;
    uint32_t *cursor=j->chain.cursor,*end=j->chain.end;
    unsigned copies=0;
    for(unsigned i=0;i<count;++i)
        copies+=cb->operations[first+i].type==PS5VK_COPY_IMAGE_BUFFER;
    if(copies>1u) {
        /* The render-pass module's readback: one target per attachment it
         * rendered. Its own validator owns the whole shape and the plans it
         * stages, one per target. */
        struct ps5vk_readback_set set={0};
        rc=ps5vk_readback_commands_set(d,cb->operations+first,count,NULL,&j->layouts,&set);
        if(rc==VK_SUCCESS) {
            j->color=set.target[0].image;
            j->readback=set;
        }
    } else if(copies) {
        struct ps5vk_readback_plan plan={0};
        rc=ps5vk_readback_commands(d,cb->operations+first,count,NULL,&j->layouts,&plan,&readback_site);
        if(rc==VK_SUCCESS) {
            j->color=plan.image;
            j->readback.count=1u;
            j->readback.target[0]=plan;
        }
    } else rc=ps5vk_upload_commands(d,cb->operations+first,count,NULL,&j->layouts,&cursor,end,cache);
    if(rc!=VK_SUCCESS) {
        /* Keep a refused upload measurable at the same boundary where it is
         * rejected. The operation index and shape tell an unsupported CTS
         * barrier from a copy-plan or resource-span failure without guessing
         * from the later queue error. */
        for(unsigned i=0;i<count;++i) {
            const struct ps5vk_operation *op=&cb->operations[first+i];
            if(op->type==PS5VK_IMAGE_BARRIER) {
                VkImage image=op->image_barrier.image;
                ps5log_printf(PS5LOG_INFO,
                    "PS5VK_UPLOAD_OP index=%u type=%u stages=%08x/%08x access=%08x/%08x layout=%u/%u format=%u usage=%08x",
                    i,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                    (unsigned)op->src_access,(unsigned)op->dst_access,
                    (unsigned)op->image_barrier.oldLayout,(unsigned)op->image_barrier.newLayout,
                    image?(unsigned)image->info.format:0u,
                    image?(unsigned)image->info.usage:0u);
            } else if(op->type==PS5VK_COPY_BUFFER_IMAGE) {
                ps5log_printf(PS5LOG_INFO,
                    "PS5VK_UPLOAD_OP index=%u type=%u stages=%08x/%08x layout=%u format=%u usage=%08x offset=%llu extent=%ux%ux%u row=%u image_height=%u",
                    i,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                    (unsigned)op->copy_layout,op->copy_image?(unsigned)op->copy_image->info.format:0u,
                    op->copy_image?(unsigned)op->copy_image->info.usage:0u,
                    (unsigned long long)op->copy_region.bufferOffset,
                    op->copy_region.imageExtent.width,op->copy_region.imageExtent.height,
                    op->copy_region.imageExtent.depth,op->copy_region.bufferRowLength,
                    op->copy_region.bufferImageHeight);
            } else {
                ps5log_printf(PS5LOG_INFO,
                    "PS5VK_UPLOAD_OP index=%u type=%u stages=%08x/%08x access=%08x/%08x buffer=%u",
                    i,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                    (unsigned)op->src_access,(unsigned)op->dst_access,
                    op->buffer_barrier.buffer?1u:0u);
            }
        }
    }
    if(rc!=VK_SUCCESS)goto fail;
    j->chain.cursor=cursor;
    rc=ps5vk_draw_batch_close(&j->chain);
    if(rc!=VK_SUCCESS)goto fail;
    j->words=j->chain.words[0];*out=j;
    ps5log_printf(PS5LOG_MARK,"PS5VK_UPLOAD_PREPARED serial=%llu operations=%u words=%u readback=%u",
        (unsigned long long)j->serial,count,j->words,j->readback.count);
    return VK_SUCCESS;
fail:
    ps5log_printf(PS5LOG_ERR,"PS5VK_UPLOAD_PREPARE_FAILED serial=%llu site=%u rc=%d",
        (unsigned long long)j->serial,readback?readback_site:0u,rc);
    release(d,j);return rc;
}
/* The bounded shapes this executor accepts, and the reason a submission that
 * is not one of them never reaches a job. A refusal here is a submission the
 * driver refuses AFTER recording accepted it, which the queue turns into a
 * lost device; naming it in the log is what makes such a refusal measurable
 * instead of silent. */
static VkResult prepare_shape(VkDevice d,const struct ps5vk_submission *s,void **out);
static VkResult prepare(VkDevice d,const struct ps5vk_submission *s,void **out)
{
    VkResult rc=prepare_shape(d,s,out);
    if(rc!=VK_SUCCESS)
        ps5log_printf(PS5LOG_ERR,"PS5VK_GRAPHICS_PREPARE_REFUSED serial=%llu rc=%d",
            (unsigned long long)(s?s->serial:0u),(int)rc);
    return rc;
}
/* The resolve draw (DXVK262-T06).
 *
 * A pass whose subpass declares a resolve target promises that the resolved
 * result reaches that target. This profile produces it with the one mechanism
 * it has measured: a draw that reads every sample of the multisampled colour
 * attachment through the resource-only record and writes their average into the
 * single-sample target. The draw belongs to the DRIVER - its stages are the ones
 * tools/build_resolve_shaders.py generates and native/resolve_program.c
 * compiles - and everything it renders through is built here on the stack: a
 * one-subpass synthetic pass whose colour reference is the resolve attachment,
 * a framebuffer carrying both views, a pipeline carrying the resolve program
 * with no blending, and a descriptor set whose single element is the
 * multisampled attachment as an input attachment. Only the shape this profile
 * measured is served; every other resolve shape keeps its refusal.
 */
static VkResult resolve_draw_emit(VkDevice d, struct graphics_job *j,
    uint32_t **cursor_io, uint32_t **end_io, const struct ps5vk_operation *begin,
    uint32_t subpass, const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],
    unsigned *site)
{
    VkRenderPass pass = begin->render_pass;
    const struct ps5vk_subpass *stage = ps5vk_render_pass_subpass(pass, subpass);
    VkFramebuffer fb = begin->framebuffer;
    /* Declared here rather than where they are filled in: the handover to the
     * job at `done` has to see an initialised (or explicitly null) value on
     * every path out of this function. */
    struct ps5vk_prepared_draw *prepared = NULL;
    void *loaded_state = NULL;
    if (!stage || !ps5vk_subpass_uses_resolve(stage)) return VK_SUCCESS;
    /* One colour reference with a real resolve target: the shape the oracle
     * builds and the only one this emission describes. */
    if (stage->color_count != 1u || stage->resolve_count != 1u ||
        stage->resolve[0].attachment == VK_ATTACHMENT_UNUSED)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint32_t colour_index = stage->color[0].attachment;
    const uint32_t resolve_index = stage->resolve[0].attachment;
    if (!fb || colour_index >= fb->attachment_count || resolve_index >= fb->attachment_count)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImageView colour_view = fb->attachments[colour_index];
    VkImageView resolve_view = fb->attachments[resolve_index];
    if (!colour_view || !resolve_view || !colour_view->image || !resolve_view->image)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkSampleCountFlagBits samples = colour_view->image->info.samples;
    if (!ps5vk_sample_count_implemented(samples) || samples == VK_SAMPLE_COUNT_1_BIT ||
        resolve_view->image->info.samples != VK_SAMPLE_COUNT_1_BIT)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const unsigned sample_count = ps5vk_sample_count_number(samples);
    struct ps5vk_resolve_program program = {0};
    VkResult rc = ps5vk_resolve_program_acquire(d, samples, &program);
    if (rc != VK_SUCCESS) { *site = 33u; return rc; }

    const VkViewport viewport = {0.0f, 0.0f, (float)fb->width, (float)fb->height, 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {fb->width, fb->height}};
    struct ps5vk_subpass synthetic_subpass = {
        .color[0] = {.attachment = 1u, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        .color_count = 1u, .resolve_count = 0u,
        .depth = {.attachment = VK_ATTACHMENT_UNUSED}};
    struct VkRenderPass_T synthetic_pass = {.device = d, .attachment_count = 2u,
        .subpass_count = 1u, .subpasses = &synthetic_subpass};
    struct VkFramebuffer_T synthetic_fb = {.device = d, .width = fb->width,
        .height = fb->height, .attachment_count = 2u, .color_count = 1u,
        .depth_attachment = VK_ATTACHMENT_UNUSED};
    synthetic_fb.attachments[0] = colour_view;
    synthetic_fb.attachments[1] = resolve_view;
    synthetic_fb.formats[0] = colour_view->format;
    synthetic_fb.formats[1] = resolve_view->format;
    synthetic_fb.samples[0] = samples;
    synthetic_fb.samples[1] = VK_SAMPLE_COUNT_1_BIT;
    synthetic_fb.color_attachments[0] = 1u;
    struct VkDescriptorPool_T synthetic_pool = {.device = d};
    struct VkDescriptorSet_T synthetic_set = {.pool = &synthetic_pool};
    synthetic_set.signature.count = 1u;
    synthetic_set.signature.binding[0] = (struct ps5vk_binding){.count = 1u, .first = 0u,
        .stages = VK_SHADER_STAGE_FRAGMENT_BIT};
    synthetic_set.signature.type[0] = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    for (unsigned b = 1; b < PS5VK_MAX_BINDINGS; ++b)
        synthetic_set.signature.binding[b].first = 1u;
    synthetic_set.defined[0] = VK_TRUE;
    synthetic_set.images[0].imageView = colour_view;
    synthetic_set.images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    synthetic_set.image_resources[0] = colour_view->image;
    /* The program pair is a COMPILED program; the draw state and the emitter
     * need the LOADED one - code uploaded, linked, runtime headers filled -
     * which is what the app path builds when it creates a graphics pipeline.
     * The resolve draw uses the same loader, so its stages go through exactly
     * the path every other pipeline's do. */
    const struct ps5vk_runtime_graphics_program *pair = program.pair;
    rc = ps5vk_native_runtime_graphics_create(d, pair, pair->primitive_type, &loaded_state);
    if (rc != VK_SUCCESS) {
        *site = 36u;
        ps5vk_resolve_program_release(d, &program);
        return rc;
    }
    struct ps5vk_native_graphics_pipeline *synthetic_state = loaded_state;
    struct VkPipeline_T synthetic_pipeline = {.device = d, .graphics = VK_TRUE,
        .graphics_usage_known = VK_TRUE, .set_count = 1u, .samples = VK_SAMPLE_COUNT_1_BIT,
        .viewport_count = 1u, .viewport = viewport, .scissor = scissor,
        .color_attachment_count = 1u, .color_format = {VK_FORMAT_R8G8B8A8_UNORM},
        .color_write_mask = {0xfu}, .front_face = VK_FRONT_FACE_CLOCKWISE,
        /* CB_TARGET_MASK is carried from the pipeline's BLEND block, not from
         * the write-mask array: ps5vk_native_draw_state reads
         * color_blend[attachment].colorWriteMask, so a synthetic pipeline that
         * sets only color_write_mask hands the hardware a target mask of ZERO -
         * the draw runs and every colour write is discarded, which is exactly
         * how the first emitted resolve managed to leave its target at the
         * clear. Both fields are set here, the way vkCreateGraphicsPipelines
         * sets both. */
        .color_blend = {{.colorWriteMask = 0xfu}},
        .graphics_state = synthetic_state};
    synthetic_pipeline.sets[0] = synthetic_set.signature;
    /* The count is not decoration: `ps5vk_native_emit_runtime_draw` treats a
     * zero vertex or instance count as Vulkan's "no rasterization side
     * effects" draw and returns success WITHOUT writing a single word - which
     * is exactly how the first emission of this draw managed to log a resolve
     * and leave the target untouched. The resolve program's vertex stage is the
     * oversized triangle every runtime pipeline uses, so the emission is three
     * vertices, one instance. */
    struct ps5vk_operation op = {.type = PS5VK_DRAW, .pipeline = &synthetic_pipeline,
        .framebuffer = &synthetic_fb, .render_pass = &synthetic_pass,
        .vertex_count = 3u, .instance_count = 1u,
        .viewport_count = 1u, .viewport = viewport, .scissor = scissor};
    op.sets[0] = &synthetic_set;
    op.generations[0] = synthetic_set.generation;
    /* The prepared draw lives in the JOB, not on this frame: its context block
     * is referenced by address from the command stream and is read by the GPU
     * long after this walk returns. */
    if (j->resolve_count >= PS5VK_MAX_SUBPASSES) { *site = 41u; rc = VK_ERROR_TOO_MANY_OBJECTS; goto done; }
    prepared = &j->resolve_draws[j->resolve_count];
    const VkRect2D area = {{0, 0}, {fb->width, fb->height}};
    /* The three ingredients of the synthetic draw are checked one at a time so
     * a refusal names which of them the profile cannot describe. */
    {
        struct ps5vk_target_set probe_set;
        struct ps5vk_target_registers probe_regs;
        uint32_t probe_words[8];
        ps5log_printf(PS5LOG_MARK, "PS5VK_RESOLVE_STEP target_set=%d",
            (int)ps5vk_target_set_from_subpass(&synthetic_pass, &synthetic_fb, 0, &probe_set));
        ps5log_printf(PS5LOG_MARK, "PS5VK_RESOLVE_STEP native_target=%d",
            (int)ps5vk_native_target(d, resolve_view, defaults, &probe_regs));
        ps5log_printf(PS5LOG_MARK, "PS5VK_RESOLVE_STEP descriptor=%d",
            (int)ps5vk_image_resource_descriptor(d, colour_view, probe_words));
    }
    /* The runtime entry, with the LOADED pair's address as the shader
     * aperture: the legacy entry passes zero there, which the descriptor
     * table's high-address check then refuses - the same call the draw loop
     * makes for every runtime-shaded draw. */
    {
        struct ps5vk_target_set resolve_targets = {0};
        resolve_targets.color_count = 1u;
        resolve_targets.color[0] = 1u;
        resolve_targets.depth = VK_ATTACHMENT_UNUSED;
        rc = ps5vk_native_prepare_resource_draw(d, &op, &area, defaults,
            (uintptr_t)synthetic_state->pair, &resolve_targets, prepared);
    }
    if (rc != VK_SUCCESS) {
        *site = 34u;
        ps5log_printf(PS5LOG_ERR, "PS5VK_RESOLVE_STEP prepare_draw rc=%d inner=%u",
            (int)rc, ps5vk_draw_prepare_site);
    }
    if (rc == VK_SUCCESS) {
        uint32_t tables[PS5VK_RUNTIME_DESCRIPTOR_SETS] = {0};
        tables[0] = (uint32_t)(uintptr_t)prepared->descriptor_tables[0];
        uint32_t *cursor = *cursor_io, *end = *end_io;
        /* The multisampled attachment has to be readable by the resolve draw,
         * which is the same transition the fetch path's boundary emits - and it
         * has to have ARRIVED, not merely been requested: the barrier writes a
         * completion token and waits for it, because the colour block writes
         * back asynchronously and a texture read issued behind the event saw
         * tiles the block had not written yet (measured: only some 8x8 tiles of
         * a resolved target carried the drawn value, different tiles per run). */
        {
            /* Room for the barrier, the boundary after it and the emission,
             * asked of the chain the way the draw loop asks for its own: the
             * chain seals the open arena and opens the next one when a whole
             * emission would not fit. */
            j->chain.cursor = cursor;
            rc = ps5vk_draw_batch_reserve(&j->chain,
                PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WAIT_WORDS + PS5VK_GRAPHICS_ACQUIRE_WORDS);
            if (rc != VK_SUCCESS) *site = 39u;
            else {
                cursor = j->chain.cursor;
                end = j->chain.end;
                volatile uint64_t *token_slot = ps5vk_draw_batch_open_label(&j->chain);
                size_t barrier = 0;
                if (!token_slot) { rc = VK_ERROR_UNKNOWN; *site = 35u; }
                else {
                    /* The wait compares for equality, so the word it polls must
                     * not already hold this submission's value: the arena is
                     * CPU-mapped and re-used, and the token starts at zero. */
                    token_slot[7] = 0;
                    barrier = ps5vk_graphics_color_to_texture_wait(cursor,
                        (size_t)(end - cursor), (uintptr_t)(token_slot + 7),
                        ++j->barrier_tokens);
                    if (!barrier) { rc = VK_ERROR_UNKNOWN; *site = 35u; }
                    else cursor += barrier;
                }
                if (rc == VK_SUCCESS)
                    rc = ps5vk_native_emit_runtime_draw(&cursor, (uint32_t)(end - cursor),
                        prepared->state, prepared->state, prepared->bytes, &op,
                        (uint32_t)(uintptr_t)prepared->vertex_table, tables, NULL, NULL,
                        sceAgcDcbDrawIndex);
                if (rc != VK_SUCCESS) *site = 40u;
                else
                    ps5log_printf(PS5LOG_MARK,
                        "PS5VK_RESOLVE_DRAW serial=%llu subpass=%u samples=%u colour=%u resolve=%u "
                        "targets=%u words=%zu",
                        (unsigned long long)j->serial, subpass,
                        (unsigned)sample_count, colour_index, resolve_index, 1u,
                        (size_t)(cursor - *cursor_io));
            }
        }
        *cursor_io = cursor;
        *end_io = end;
    }
    /* The job owns the prepared draw AND the loaded pair from here: the command
     * stream references the context block by address and the context references
     * the uploaded shader code, so both are read by the GPU after this walk has
     * returned. Handing them over is what makes the draw land. */
    if (rc == VK_SUCCESS && prepared->state) {
        j->resolve_states[j->resolve_count] = loaded_state;
        ++j->resolve_count;
        loaded_state = NULL;
    } else if (prepared->state) {
        ps5vk_native_release_draw(prepared);
    }
done:
    if (loaded_state) ps5vk_native_graphics_release(d, loaded_state);
    ps5vk_resolve_program_release(d, &program);
    return rc;
}

static VkResult prepare_shape(VkDevice d,const struct ps5vk_submission *s,void **out)
{
    /* The early refusals below report the line they fired on, because a
     * submission that a shape gate rejects leaves no other trace. */
    unsigned site_report=0;
    VkResult rc=VK_SUCCESS;
    struct graphics_job *j=NULL;
    /* The phase the preparation is in, logged as it is entered. A preparation
     * that does not return leaves no failure record of its own, so the last
     * phase in the log is what names the step a crash happened in - the same
     * reasoning the shape walk's step log uses. */
    const char *phase="shape";
#define PHASE(name) do { phase=(name);                                            \
        ps5log_printf(PS5LOG_MARK,"PS5VK_GRAPHICS_PHASE serial=%llu phase=%s",     \
            (unsigned long long)j->serial,phase); } while(0)
    /* Which refusal inside the draw phase fired. Every one of them returns the
     * same error code from a different line, so a failure reports "phase=draw"
     * and nothing else - which cost a window per gate while opening the
     * descriptor profile for a diagnostic. Numbering them makes one run name
     * the line. Zero means the failure was not one of the numbered sites. */
    unsigned draw_site=0;
    *out=NULL;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_GRAPHICS_PREPARE_ENTER serial=%llu buffers=%u first=%u count=%u",
        (unsigned long long)(s?s->serial:0),s?s->count:0,
        s?ps5vk_submission_first_operation(s,0):0,
        s?ps5vk_submission_operation_count(s,0):0);
    /* One color pass, optional D32 and texture-upload prelude.  LOAD preserves
     * an attachment only when its tracked initial layout matches.  CLEAR is
     * bounded to the full render area until a rectangular clear path exists. */
    /* A submission is an ordered list of segments, one per primary command
     * buffer it names: the transfer work the pinned render-pass module records
     * before its pass, the pass itself (whose work secondaries may NAMED, in
     * recorded order), and the readback it records after. Exactly one buffer
     * may carry a render pass - the executor builds one - and every other
     * buffer's range must be transfer work the same prelude and postlude
     * emitters already serve. */
    if(!s->count || s->count>PS5VK_MAX_SUBMITTED_BUFFERS || !s->serial)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    unsigned pass_buffer=s->count;
    for(unsigned i=0;i<s->count;++i) {
        VkCommandBuffer listed=s->buffers[i];
        uint32_t listed_first=ps5vk_submission_first_operation(s,i);
        uint32_t listed_count=ps5vk_submission_operation_count(s,i);
        if(!listed || !listed_count || listed->operation_count>PS5VK_MAX_OPERATIONS ||
           listed_first>listed->operation_count ||
           listed_count>listed->operation_count-listed_first)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        for(uint32_t k=listed_first;k<listed_first+listed_count;++k)
            if(listed->operations[k].type==PS5VK_BEGIN_RENDER_PASS) {
                /* Two passes in one submission, or a pass named twice, is a
                 * graph this executor does not build. */
                if(pass_buffer!=s->count)return VK_ERROR_FEATURE_NOT_PRESENT;
                pass_buffer=i;
            }
    }
    /* No render pass anywhere in the submission: a transfer-only segment
     * names one buffer, which the transfer path owns end to end. */
    if(pass_buffer==s->count) {
        if(s->count!=1)return VK_ERROR_FEATURE_NOT_PRESENT;
        return prepare_transfer(d,s,s->buffers[0],
            ps5vk_submission_first_operation(s,0),
            ps5vk_submission_operation_count(s,0),out);
    }
    VkCommandBuffer cb=s->buffers[pass_buffer];
    uint32_t range_first=ps5vk_submission_first_operation(s,pass_buffer);
    uint32_t range_count=ps5vk_submission_operation_count(s,pass_buffer);
    uint32_t range_end=range_first+range_count;
    unsigned first=range_first;
    while(first<range_end && cb->operations[first].type!=PS5VK_BEGIN_RENDER_PASS) {
        unsigned type=cb->operations[first].type;
        if(type!=PS5VK_BARRIER && type!=PS5VK_IMAGE_BARRIER &&
           type!=PS5VK_COPY_BUFFER_IMAGE && type!=PS5VK_COPY_IMAGE_BUFFER &&
           type!=PS5VK_CLEAR_DEPTH_STENCIL_IMAGE && type!=PS5VK_CLEAR_COLOR_IMAGE)
            {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        ++first;
    }
    unsigned last=first+1;
    while(last<range_end && cb->operations[last].type!=PS5VK_END_RENDER_PASS)++last;
    /* last<first+2 is a pass with no work between begin and end. Recording
     * already refuses that shape, so this is a defence in depth on the
     * immutable record rather than a boundary a caller can reach: nothing is
     * accepted at record time and rejected here. */
    if(first>=range_end || last>=range_end || last<first+2)
        {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
    const struct ps5vk_operation *begin=&cb->operations[first]; VkRenderPass pass=begin->render_pass;
    /* Execute the shared-role profile: ordered subpasses using the
     * same color/depth attachments and layouts. Wider graphs, or graphs that
     * would need attachment rebinding/layout changes, remain fail-closed. */
    if(!pass->subpass_count || pass->subpass_count>PS5VK_MAX_SUBPASSES){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
    const struct ps5vk_subpass *subpass=ps5vk_render_pass_subpass(pass,0);
    int depth=subpass->depth.attachment!=VK_ATTACHMENT_UNUSED;
    /* A resolve target is SERVED now (DXVK262-T06): the per-subpass validation
     * below admits the shape it can emit and refuses every other one with the
     * same code, so this earlier blanket refusal is gone. */
    /* A preserve list is a promise this queue CAN keep now that every subpass
     * renders its own targets (DXVK262-T06): a preserved attachment is one the
     * subpass does not write, the pass's load ops are applied once at its
     * start, and the boundary between subpasses publishes the previous
     * subpass's writes without touching anything else, so the contents a
     * subpass preserves come out of it unchanged. The list is validated here
     * anyway - the object model already refused a reference outside the pass -
     * so a record that reached this backend with one is refused rather than
     * read out of bounds. */
    for(uint32_t s=0;s<pass->subpass_count;++s) {
        const struct ps5vk_subpass *sp=ps5vk_render_pass_subpass(pass,s);
        const uint32_t *list=ps5vk_render_pass_preserves(pass,s);
        for(uint32_t k=0;k<sp->preserve_count;++k)
            if(list[k]>=pass->attachment_count){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
    }
    /* Every subpass may name its OWN colour attachments (DXVK262-T06). What a
     * draw renders into is its subpass's own set - the seam the draw
     * preparation takes - so the pass carries the UNION of those references,
     * while the depth role stays single and shared: one depth surface, one
     * layout, for every subpass that names one. A resolve target and a preserve
     * list are still refused above, and an attachment named twice has to agree
     * with itself on format and layout. */
    uint32_t color_count=0;
    uint32_t colour_attachment[PS5VK_MAX_ATTACHMENTS];
    VkFormat colour_format[PS5VK_MAX_ATTACHMENTS]={0};
    VkImageLayout colour_layout[PS5VK_MAX_ATTACHMENTS]={0};
    int colour_role[PS5VK_MAX_ATTACHMENTS]={0};
    uint32_t depth_attachment=VK_ATTACHMENT_UNUSED,depth_subpass=0;
    VkImageLayout depth_layout=VK_IMAGE_LAYOUT_UNDEFINED;
    for(uint32_t s=0;s<pass->subpass_count;++s) {
        const struct ps5vk_subpass *sp=ps5vk_render_pass_subpass(pass,s);
        if(sp->color_count>PS5VK_MAX_COLOR_ATTACHMENTS ||
           (!sp->color_count && sp->depth.attachment==VK_ATTACHMENT_UNUSED))
            {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        /* A resolve target is served now (DXVK262-T06), but only in the shape
         * the emission describes: one colour reference with one real resolve
         * target, rendered from a multisampled attachment at a served count
         * into a single-sample target. Anything else keeps the refusal. */
        if(ps5vk_subpass_uses_resolve(sp)) {
            if(sp->color_count!=1u || sp->resolve_count!=1u ||
               sp->resolve[0].attachment==VK_ATTACHMENT_UNUSED ||
               sp->resolve[0].attachment>=pass->attachment_count ||
               sp->color[0].attachment>=pass->attachment_count)
                {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
            const VkImage colour_image=begin->framebuffer->attachments[sp->color[0].attachment]->image;
            const VkImage resolve_image=begin->framebuffer->attachments[sp->resolve[0].attachment]->image;
            if(!ps5vk_sample_count_implemented(colour_image->info.samples) ||
               colour_image->info.samples==VK_SAMPLE_COUNT_1_BIT ||
               resolve_image->info.samples!=VK_SAMPLE_COUNT_1_BIT ||
               colour_image->info.format!=resolve_image->info.format)
                {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        }
        for(uint32_t c=0;c<sp->color_count;++c) {
            const uint32_t a=sp->color[c].attachment;
            if(a>=pass->attachment_count){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
            const VkFormat format=begin->framebuffer->attachments[a]->image->info.format;
            if(!ps5vk_color_target_format_supported(format)){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
            if(colour_role[a]) {
                if(colour_format[a]!=format || colour_layout[a]!=sp->color[c].layout)
                    {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
                continue;
            }
            colour_role[a]=1;
            colour_format[a]=format;
            colour_layout[a]=sp->color[c].layout;
            colour_attachment[color_count++]=a;
        }
        if(sp->depth.attachment!=VK_ATTACHMENT_UNUSED) {
            if(sp->depth.attachment>=pass->attachment_count){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
            if(depth_attachment!=VK_ATTACHMENT_UNUSED &&
               (depth_attachment!=sp->depth.attachment || depth_layout!=sp->depth.layout ||
                pass->stencil.reference[s]!=pass->stencil.reference[depth_subpass]))
                {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
            if(depth_attachment==VK_ATTACHMENT_UNUSED)depth_subpass=s;
            depth_attachment=sp->depth.attachment;
            depth_layout=sp->depth.layout;
        }
    }
    if(!color_count && depth_attachment==VK_ATTACHMENT_UNUSED)
        {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
    depth=depth_attachment!=VK_ATTACHMENT_UNUSED;
    /* Every forward edge is covered by flushing CB and acquiring at each
     * intervening boundary, across all views. Self-dependencies only declare
     * allowed in-pass scopes; they do not schedule work by themselves. */
    for(uint32_t i=0;i<pass->dependency_count;++i) {
        const VkSubpassDependency *dep=&pass->dependencies[i];
        if(dep->srcSubpass>=pass->subpass_count ||
           dep->dstSubpass>=pass->subpass_count ||
           dep->srcSubpass>dep->dstSubpass)
            {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
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
            {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        for(uint32_t k=0;k<multiview->dependency_count;++k)
            if(multiview->view_offsets[k]){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
    }
    struct ps5vk_attachment_plan color_plan[PS5VK_MAX_ATTACHMENTS]={{0}},depth_plan={0};
    /* The combined depth/stencil attachment: one plan and one layout
     * sequence per aspect (VK_KHR_separate_depth_stencil_layouts). */
    struct ps5vk_depth_stencil_plan ds_plan={0};
    struct ps5vk_depth_stencil_layouts ds_initial={0},ds_reference={0},ds_final={0};
    int combined_depth=0;
    /* One clear word per colour attachment: the ordered clear before the pass is
     * a whole-surface DMA fill per target, and two attachments may legitimately
     * ask for different values. The plan is per ATTACHMENT, because a subpass
     * that renders elsewhere renders a different surface. */
    uint32_t clear_word[PS5VK_MAX_ATTACHMENTS]={0};
    /* The counts the platform this device was created from serves, so a
     * multisampled attachment is executable exactly on a build whose mask
     * carries the sample-rate bit (DXVK262-T06). */
    const VkSampleCountFlags served_samples=
        ps5vk_platform_sample_counts(d->platform_features);
    for(uint32_t k=0;k<color_count;++k) {
        const uint32_t a=colour_attachment[k];
        /* The pinned render-pass module reads every attachment of its pass
         * back, so its colour attachments end the pass in the transfer-source
         * layout. The role predicate bounds which images may do that. */
        VkImage target=begin->framebuffer->attachments[a]->image;
        const VkBool32 readback=(VkBool32)(ps5vk_colour_transfer_image(target) &&
            (target->info.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        if(ps5vk_attachment_plan(&pass->attachments[a],colour_format[a],
            colour_layout[a],VK_FALSE,readback,served_samples,&color_plan[a])!=VK_SUCCESS)
            {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        if(!color_plan[a].clear) continue;
        VkImage image=target;
        /* An integer target's clear is the raw 32-bit word its components
         * pack into, not a UNORM conversion. */
        int clear_ok=colour_format[a]==VK_FORMAT_R8G8B8A8_SINT ?
            ps5vk_color_clear_rgba8_sint(begin->clears[a].color.int32,&clear_word[a]) :
            ps5vk_color_target_integer_served(colour_format[a]) ?
            ps5vk_color_clear_rgba8_uint(begin->clears[a].color.uint32,&clear_word[a]) :
            (colour_format[a]==VK_FORMAT_B8G8R8A8_UNORM ?
                ps5vk_color_clear_bgra8(begin->clears[a].color.float32,&clear_word[a]) :
                ps5vk_color_clear_rgba8(begin->clears[a].color.float32,&clear_word[a]));
        if(image->info.format!=colour_format[a] || begin->clear_count<=a || !clear_ok ||
           begin->render_area.offset.x || begin->render_area.offset.y ||
           begin->render_area.extent.width!=image->info.extent.width ||
           begin->render_area.extent.height!=image->info.extent.height) {
            rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;
        }
    }
    if(depth) {
        const uint32_t a=depth_attachment;
        VkImage image=begin->framebuffer->attachments[a]->image;
        combined_depth=ps5vk_format_is_combined_depth_stencil(image->info.format);
        if(combined_depth) {
            if(!ps5vk_render_pass_depth_stencil_layouts(pass,depth_subpass,a,
                   &ds_initial.depth,&ds_initial.stencil,&ds_reference.depth,
                   &ds_reference.stencil,&ds_final.depth,&ds_final.stencil) ||
               ps5vk_depth_stencil_attachment_plan(&pass->attachments[a],image->info.format,
                   &ds_initial,&ds_reference,&ds_final,&ds_plan)!=VK_SUCCESS)
                {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
            /* The depth half keeps the single-aspect plan's meaning for the
             * clear below; the stencil half has its own. */
            depth_plan=(struct ps5vk_attachment_plan){ds_plan.depth_clear,
                ds_plan.depth_load,ds_plan.depth_store};
        }
        if((!combined_depth && ps5vk_attachment_plan(&pass->attachments[a],image->info.format,depth_layout,
            VK_TRUE,VK_FALSE,served_samples,&depth_plan)!=VK_SUCCESS) ||
            image->info.extent.width!=begin->framebuffer->width ||
            image->info.extent.height!=begin->framebuffer->height){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        if(depth_plan.clear) {
            float clear=begin->clears[a].depthStencil.depth;
            uint32_t ignored;
            if(begin->clear_count<=a ||
                !ps5vk_depth_attachment_clear_word(image->info.format,clear,&ignored) ||
                begin->render_area.offset.x || begin->render_area.offset.y ||
                begin->render_area.extent.width!=begin->framebuffer->width ||
                begin->render_area.extent.height!=begin->framebuffer->height)
                {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        }
        /* A stencil clear is a whole-plane fill as well, so it needs the
         * whole render area just like the depth one. */
        if(ds_plan.stencil_clear &&
           (begin->clear_count<=a ||
            begin->render_area.offset.x || begin->render_area.offset.y ||
            begin->render_area.extent.width!=begin->framebuffer->width ||
            begin->render_area.extent.height!=begin->framebuffer->height))
            {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
    }
    /* Ordered body of the pass. vkCmdExecuteCommands carries no work of its
     * own: it NAMES children, and each name expands here into that child's own
     * recorded draws, taken from the child's own buffer. Nothing was flattened
     * into the primary at record time, so this is where the one command stream
     * the pass needs is assembled - and only here, from buffers the submission
     * itself names, matched by identity so this code cannot invent the
     * association. */
    const struct ps5vk_operation *body[PS5VK_MAX_OPERATIONS];
    unsigned body_count=0,next_buffer=pass_buffer+1;
    for(unsigned i=first+1;i<last;++i) {
        const struct ps5vk_operation *op=&cb->operations[i];
        if(op->type==PS5VK_NEXT_SUBPASS || op->type==PS5VK_CLEAR_ATTACHMENT) {
            if(body_count==PS5VK_MAX_OPERATIONS){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
            body[body_count++]=op;
            continue;
        }
        if(op->type==PS5VK_EXECUTE_COMMANDS) {
            VkCommandBuffer const *children=(VkCommandBuffer const *)op->owned_payload;
            if(!children || !op->child_count ||
               op->owned_payload_size!=(size_t)op->child_count*sizeof(*children))
                {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
            for(uint32_t n=0;n<op->child_count;++n) {
                if(next_buffer>=s->count || s->buffers[next_buffer]!=children[n])
                    {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
                VkCommandBuffer child=s->buffers[next_buffer];
                uint32_t child_first=ps5vk_submission_first_operation(s,next_buffer);
                uint32_t child_count=ps5vk_submission_operation_count(s,next_buffer);
                if(child->operation_count>PS5VK_MAX_OPERATIONS ||
                   child_first>child->operation_count ||
                   child_count>child->operation_count-child_first)
                    {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
                for(uint32_t k=child_first;k<child_first+child_count;++k) {
                    const struct ps5vk_operation *inner=&child->operations[k];
                    if(inner->type!=PS5VK_DRAW && inner->type!=PS5VK_DRAW_INDEXED &&
                       !ps5vk_indirect_graphics_operation(inner->type) &&
                       inner->type!=PS5VK_QUERY_BEGIN && inner->type!=PS5VK_QUERY_END)
                        {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
                    /* The prepared-draw arena is bounded; refuse rather than
                     * silently dropping the tail of the pass. */
                    if(body_count==PS5VK_MAX_OPERATIONS){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
                    body[body_count++]=inner;
                }
                ++next_buffer;
            }
            continue;
        }
        if(op->type!=PS5VK_DRAW && op->type!=PS5VK_DRAW_INDEXED &&
           !ps5vk_indirect_graphics_operation(op->type) &&
           op->type!=PS5VK_QUERY_BEGIN && op->type!=PS5VK_QUERY_END)
            {rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        if(body_count==PS5VK_MAX_OPERATIONS){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
        body[body_count++]=op;
    }
    /* Every named buffer must have been consumed by a name in this pass, and
     * the pass must actually carry work: naming only empty secondaries expands
     * to no draws at all, which is the zero-body shape recording already
     * refuses. Defence in depth behind that check and the submission-time one,
     * on the immutable record this backend is handed. */
    if(next_buffer!=s->count || !body_count){rc=VK_ERROR_FEATURE_NOT_PRESENT;site_report=__LINE__;goto fail;}
    j=calloc(1,sizeof(*j)); if(!j)return VK_ERROR_OUT_OF_HOST_MEMORY;
    j->serial=s->serial;
    /* The image the prelude and postlude act on. A depth-only pass has no
     * colour image, and the helpers below already treat a missing one as
     * "no colour work in this range" rather than as an error. */
    j->color=color_count?begin->framebuffer->attachments[colour_attachment[0]]->image:NULL;
    int needs_query_arena=0;
    for(unsigned i=0;i<body_count;++i)
        if(body[i]->type==PS5VK_QUERY_BEGIN) needs_query_arena=1;
    if(needs_query_arena) {
        phase="query-arena";
        rc=ps5vk_command_arena_create(&j->query_arena);
        if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
        j->query_arena_active=1;
        memset(j->query_arena.address,0,
            (size_t)PS5VK_QUERY_SLOTS*PS5VK_QUERY_SLOT_BYTES);
        cache(j->query_arena.address,
            (size_t)PS5VK_QUERY_SLOTS*PS5VK_QUERY_SLOT_BYTES);
    }
    PHASE("command-arena");
    rc=ps5vk_draw_batch_open(&j->chain,j->serial);
    if(rc==VK_ERROR_DEVICE_LOST)retain("command-create");
    if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
    /* Every emission below writes through this cursor pair and asks the chain
     * for room first; the chain seals the open arena and opens the next one
     * when a whole emission would not fit. */
    uint32_t *cursor=j->chain.cursor,*end=j->chain.end;
#define BATCH_RESERVE(need) do { j->chain.cursor=cursor; \
        rc=ps5vk_draw_batch_reserve(&j->chain,(need)); if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;} \
        cursor=j->chain.cursor;end=j->chain.end; } while(0)
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    /* Owned, cache-line-aligned, zeroed GPU-visible slot. Creation zeroes it,
     * which is what makes an unwritten pair read as unavailable. */
    PHASE("occlusion-slot");
    rc=ps5vk_command_arena_create(&j->slot);
    if(rc==VK_ERROR_DEVICE_LOST)retain("occlusion-slot-create");
    if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
    /* The arena is zeroed by the CPU, and this memory is non-coherent: flush
     * the zeroed lines before the GPU writes the counter pair into them, or a
     * dirty CPU line could later overwrite the hardware's words. The readback
     * side flushes again only after the exact completion label. */
    cache(j->slot.address,(size_t)PS5VK_OCCLUSION_PROBE_BYTES);
    j->slot_active=1;
#endif
    /* The segments the submission recorded BEFORE the pass belong to the same
     * command stream and the same layout transaction: the pinned module
     * initializes its attachments in a command buffer of its own, one acquire,
     * one clear and one handover per target, and the pass that follows
     * consumes exactly those layouts. */
    phase="prelude-segments";
    for(unsigned i=0;i<pass_buffer;++i) {
        VkCommandBuffer listed=s->buffers[i];
        const uint32_t listed_first=ps5vk_submission_first_operation(s,i);
        const uint32_t listed_count=ps5vk_submission_operation_count(s,i);
        j->chain.cursor=cursor;
        rc=ps5vk_upload_commands(d,listed->operations+listed_first,listed_count,NULL,
            &j->layouts,&cursor,end,cache);
        if(rc!=VK_SUCCESS)goto fail;
    }
    j->chain.cursor=cursor;
    /* Prelude transitions and clears precede the pass's attachment load
     * operations in both the command stream and the tentative layout
     * transaction. */
    phase="prelude";
    PHASE("prelude");
    rc=ps5vk_upload_commands(d,cb->operations+range_first,first-range_first,j->color,
        &j->layouts,&cursor,end,cache);
    if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
    /* Record the scoped render-pass transitions transactionally. Resource
     * state becomes committed only after the exact GPU completion label. */
    PHASE("attachment-layout");
    /* Every colour target the pass carries takes its own layout SEQUENCE, in
     * attachment order: a second target is a separate surface with its own
     * tracked layout, not part of attachment zero's. The sequence is the
     * attachment's initial layout, then the layout each subpass declares for it
     * as the pass reaches that subpass, then the attachment's final layout,
     * because that is exactly what a render pass does to an attachment - and it
     * is where the pinned multisample oracle's read layout enters: subpass 0
     * renders the multisampled colour attachment as a colour target, and the
     * fetch subpasses declare that same attachment as an input attachment in
     * SHADER_READ_ONLY_OPTIMAL. The boundary transition therefore leaves the
     * attachment in the layout the READING subpass declares, which is what the
     * input-attachment gate then checks the recorded descriptor against. */
    for(uint32_t k=0;k<color_count;++k) {
        const uint32_t a=colour_attachment[k];
        const VkImage image=begin->framebuffer->attachments[a]->image;
        VkImageLayout current=pass->attachments[a].initialLayout;
        int moved=0;
        for(uint32_t s=0;s<pass->subpass_count;++s) {
            VkImageLayout declared;
            if(!ps5vk_render_pass_attachment_layout(pass,s,a,&declared) ||
               declared==current)continue;
            rc=ps5vk_layout_transition(&j->layouts,image,current,declared);
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
            current=declared;moved=1;
        }
        if(!moved) {
            /* No subpass moved it: one transition registers the surface in the
             * transaction and lands it on the pass's final layout, which is the
             * single-layout behaviour every measured pass had before this
             * walk existed. */
            rc=ps5vk_layout_transition(&j->layouts,image,
                pass->attachments[a].initialLayout,pass->attachments[a].finalLayout);
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
            current=pass->attachments[a].finalLayout;
        } else if(current!=pass->attachments[a].finalLayout) {
            /* Whatever a middle subpass declared for it, the pass leaves the
             * attachment in that attachment's final layout. */
            rc=ps5vk_layout_transition(&j->layouts,image,current,
                pass->attachments[a].finalLayout);
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
        }
    }
    if(depth && combined_depth) {
        /* Each aspect is carried from its own initial layout to its own final
         * one; UNDEFINED discards only that aspect. */
        VkImage ds_image=begin->framebuffer->attachments[depth_attachment]->image;
        rc=ps5vk_layout_transition_aspects(&j->layouts,ds_image,VK_IMAGE_ASPECT_DEPTH_BIT,
            ds_initial.depth,ds_final.depth);
        if(rc==VK_SUCCESS)
            rc=ps5vk_layout_transition_aspects(&j->layouts,ds_image,VK_IMAGE_ASPECT_STENCIL_BIT,
                ds_initial.stencil,ds_final.stencil);
        if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
    } else if(depth) {
        rc=ps5vk_layout_transition(&j->layouts,begin->framebuffer->attachments[depth_attachment]->image,
            pass->attachments[depth_attachment].initialLayout,
            pass->attachments[depth_attachment].finalLayout);
        if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
    }
    ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT];
    if(ps5_color_select_runtime_defaults(defaults,sceAgcGetRegisterDefaults())) {rc=VK_ERROR_INITIALIZATION_FAILED;draw_site=__LINE__;goto fail;}
    /* Clear only the layers named by each attachment view. Filling the image's
     * whole allocation would erase the other faces of an array attachment. */
    for(uint32_t k=0;k<color_count;++k) {
        const uint32_t a=colour_attachment[k];
        if(!color_plan[a].clear)continue;
        void *address;VkDeviceSize bytes,stride;
        VkImageView view=begin->framebuffer->attachments[a];
        VkImage image=view->image;
        rc=ps5vk_image_span(d,image,&address,&bytes);
        if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
        rc=ps5vk_native_layer_footprint(d,image,&stride);
        if(rc!=VK_SUCCESS || !stride ||
           view->range.baseArrayLayer>=image->info.arrayLayers ||
           !view->range.layerCount ||
           view->range.layerCount>image->info.arrayLayers-view->range.baseArrayLayer ||
           stride>bytes/image->info.arrayLayers) {
            rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=__LINE__;goto fail;
        }
        const VkDeviceSize offset=(VkDeviceSize)view->range.baseArrayLayer*stride;
        const VkDeviceSize clear_bytes=(VkDeviceSize)view->range.layerCount*stride;
        cache((uint8_t *)address+offset,(size_t)clear_bytes);
        size_t n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),
            (uintptr_t)address+offset,clear_bytes,clear_word[a]);
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}cursor+=n;
        n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,"PS5VK_COLOR_CLEAR_PREPARED serial=%llu target=%u bgra=%08x bytes=%llu",
            (unsigned long long)j->serial,a,clear_word[a],(unsigned long long)clear_bytes);
    }
    if(depth && depth_plan.clear) {
        void *address;VkDeviceSize bytes;
        rc=ps5vk_image_span(d,begin->framebuffer->attachments[depth_attachment]->image,&address,&bytes);
        if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
        uint32_t value;
        VkImage depth_image=begin->framebuffer->attachments[depth_attachment]->image;
        if(!ps5vk_depth_attachment_clear_word(depth_image->info.format,
            begin->clears[depth_attachment].depthStencil.depth,&value))
            {rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=__LINE__;goto fail;}
        /* A combined surface's depth clear fills the depth plane only; the
         * stencil plane follows it and has its own load operation. */
        if(combined_depth) {
            struct ps5vk_depth_stencil_layout planes;
            if(ps5vk_depth_stencil_layout(depth_image->info.extent.width,
                   depth_image->info.extent.height,&planes) || planes.bytes>bytes)
                {rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}
            bytes=planes.depth.bytes;
        }
        cache(address,(size_t)bytes);
        size_t n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)address,bytes,value);
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}cursor+=n;
        n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}cursor+=n;
    }
    if(depth && combined_depth && ds_plan.stencil_clear) {
        /* The stencil plane holds one byte per texel, so a uniform clear is
         * the clear value's low byte repeated through every dword - tiling
         * invariant exactly like the depth fill. Vulkan takes the stencil
         * clear value's low eight bits for an 8-bit stencil aspect. */
        VkImage ds_image=begin->framebuffer->attachments[depth_attachment]->image;
        void *address;VkDeviceSize bytes;
        struct ps5vk_depth_stencil_layout planes;
        rc=ps5vk_image_span(d,ds_image,&address,&bytes);
        if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
        if(ps5vk_depth_stencil_layout(ds_image->info.extent.width,
               ds_image->info.extent.height,&planes) || planes.bytes>bytes)
            {rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}
        const uint32_t byte=begin->clears[depth_attachment].depthStencil.stencil&0xffu;
        const uint32_t word=byte*UINT32_C(0x01010101);
        uint8_t *stencil=(uint8_t *)address+planes.stencil_offset;
        cache(stencil,(size_t)planes.stencil_bytes);
        size_t n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)stencil,
            planes.stencil_bytes,word);
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}cursor+=n;
        n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,"PS5VK_STENCIL_CLEAR_PREPARED serial=%llu attachment=%u value=%02x bytes=%llu",
            (unsigned long long)j->serial,depth_attachment,byte,
            (unsigned long long)planes.stencil_bytes);
    }
    /* A RESOLVE target is an attachment of the pass like any other, so Vulkan's
     * load operation applies to it: when the pass declares CLEAR for the
     * single-sample attachment that receives a subpass's resolved result, the
     * whole surface is cleared before the pass begins. The driver used to leave
     * it to gather whatever its allocation happened to hold - which is why a
     * readback of a resolve target could show an earlier phase's pattern, and
     * why "the resolve draw wrote nothing" was indistinguishable from "the
     * resolve draw wrote somewhere else". A target a colour reference already
     * cleared is not cleared twice. */
    for(uint32_t s=0;s<pass->subpass_count;++s) {
        const struct ps5vk_subpass *sp=ps5vk_render_pass_subpass(pass,s);
        for(uint32_t r=0;r<sp->resolve_count;++r) {
            const uint32_t a=sp->resolve[r].attachment;
            if(a==VK_ATTACHMENT_UNUSED||a>=pass->attachment_count||
               a>=begin->framebuffer->attachment_count||
               !begin->framebuffer->attachments[a])continue;
            int seen=0;
            for(uint32_t k=0;k<color_count&&!seen;++k)seen=colour_attachment[k]==a;
            for(uint32_t earlier=0;earlier<s&&!seen;++earlier) {
                const struct ps5vk_subpass *previous=ps5vk_render_pass_subpass(pass,earlier);
                for(uint32_t q=0;q<previous->resolve_count&&!seen;++q)
                    seen=previous->resolve[q].attachment==a;
            }
            if(seen||pass->attachments[a].loadOp!=VK_ATTACHMENT_LOAD_OP_CLEAR)continue;
            if(begin->clear_count<=a){rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=__LINE__;goto fail;}
            VkImage image=begin->framebuffer->attachments[a]->image;
            uint32_t word;
            const int clear_ok=image->info.format==VK_FORMAT_R8G8B8A8_SINT?
                ps5vk_color_clear_rgba8_sint(begin->clears[a].color.int32,&word):
                ps5vk_color_target_integer_served(image->info.format)?
                ps5vk_color_clear_rgba8_uint(begin->clears[a].color.uint32,&word):
                (image->info.format==VK_FORMAT_B8G8R8A8_UNORM?
                    ps5vk_color_clear_bgra8(begin->clears[a].color.float32,&word):
                    ps5vk_color_clear_rgba8(begin->clears[a].color.float32,&word));
            if(!clear_ok||image->info.samples!=VK_SAMPLE_COUNT_1_BIT||
               begin->render_area.offset.x||begin->render_area.offset.y||
               image->info.extent.width!=begin->render_area.extent.width||
               image->info.extent.height!=begin->render_area.extent.height)
                {rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=__LINE__;goto fail;}
            void *address;VkDeviceSize bytes;
            rc=ps5vk_image_span(d,image,&address,&bytes);
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
            cache(address,(size_t)bytes);
            size_t n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)address,bytes,word);
            if(!n){rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}cursor+=n;
            n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
            if(!n){rc=VK_ERROR_UNKNOWN;draw_site=__LINE__;goto fail;}cursor+=n;
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_RESOLVE_CLEAR_PREPARED serial=%llu target=%u word=%08x bytes=%llu",
                (unsigned long long)j->serial,a,word,(unsigned long long)bytes);
        }
    }
    PHASE("draw");
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    if(j->slot_active) {
#if defined(PS5VK_OCCLUSION_PRECISE_PROBE) && PS5VK_OCCLUSION_PRECISE_PROBE
        size_t control=ps5vk_graphics_occlusion_control(cursor,(size_t)(end-cursor),1);
        if(!control){rc=VK_ERROR_UNKNOWN;draw_site=1;goto fail;}cursor+=control;
#endif
        size_t n=ps5vk_graphics_occlusion_event(cursor,(size_t)(end-cursor),(uint64_t)(uintptr_t)j->slot.address);
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=1;goto fail;}cursor+=n;
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_OCCLUSION_PROBE_BEGIN serial=%llu base=%llx pairs=%u precise=%u depth=%u",
            (unsigned long long)j->serial,(unsigned long long)(uintptr_t)j->slot.address,
            (unsigned)PS5VK_OCCLUSION_PROBE_PAIRS,
            (unsigned)PS5VK_OCCLUSION_PRECISE_PROBE,
            (unsigned)PS5VK_OCCLUSION_DEPTH_PROBE);
    }
#endif
    uint32_t subpass_index=0;
    int active_query_slot=-1;
    for(unsigned i=0;i<body_count;++i) {
        const struct ps5vk_operation *recorded=body[i];
        if(recorded->type==PS5VK_QUERY_BEGIN) {
            if(active_query_slot>=0 || j->query_count>=PS5VK_QUERY_SLOTS ||
               recorded->query_count!=1 || !recorded->query_pool ||
               recorded->query_first>=recorded->query_pool->query_count) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=26;goto fail;
            }
            BATCH_RESERVE(64u);
            size_t control=ps5vk_graphics_occlusion_control(cursor,
                (size_t)(end-cursor),1);
            if(!control){rc=VK_ERROR_UNKNOWN;draw_site=26;goto fail;}cursor+=control;
            uint64_t *counters=(uint64_t *)((uint8_t *)j->query_arena.address+
                (size_t)j->query_count*PS5VK_QUERY_SLOT_BYTES);
            size_t event=ps5vk_graphics_occlusion_event(cursor,
                (size_t)(end-cursor),(uint64_t)(uintptr_t)counters);
            if(!event){rc=VK_ERROR_UNKNOWN;draw_site=26;goto fail;}cursor+=event;
            j->queries[j->query_count].pool=recorded->query_pool;
            j->queries[j->query_count].query=recorded->query_first;
            j->queries[j->query_count].counters=counters;
            active_query_slot=(int)j->query_count++;
            continue;
        }
        if(recorded->type==PS5VK_QUERY_END) {
            if(active_query_slot<0 ||
               j->queries[active_query_slot].pool!=recorded->query_pool ||
               j->queries[active_query_slot].query!=recorded->query_first) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=27;goto fail;
            }
            BATCH_RESERVE(64u);
            uint64_t address=(uint64_t)(uintptr_t)
                j->queries[active_query_slot].counters+8u;
            size_t event=ps5vk_graphics_occlusion_event(cursor,
                (size_t)(end-cursor),address);
            if(!event){rc=VK_ERROR_UNKNOWN;draw_site=27;goto fail;}cursor+=event;
            size_t control=ps5vk_graphics_occlusion_control(cursor,
                (size_t)(end-cursor),0);
            if(!control){rc=VK_ERROR_UNKNOWN;draw_site=27;goto fail;}cursor+=control;
            active_query_slot=-1;
            continue;
        }
        if(recorded->type==PS5VK_CLEAR_ATTACHMENT) {
            if(recorded->subpass!=subpass_index || !ps5vk_clear_attachment_valid(recorded)) {
                rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=2;goto fail;
            }
            if(ps5vk_d16_attachment_image(recorded->image_destination)) {
                void *address;VkDeviceSize bytes;
                rc=ps5vk_image_span(d,recorded->image_destination,&address,&bytes);
                if(rc!=VK_SUCCESS || bytes!=65536u) {
                    rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=3;goto fail;
                }
                BATCH_RESERVE(PS5VK_DRAW_BATCH_INITIAL_RESERVE*2u);
                size_t n=ps5vk_graphics_release_wait(cursor,(size_t)(end-cursor),
                    (uintptr_t)(ps5vk_draw_batch_open_label(&j->chain)+7),i+1u);
                if(!n){rc=VK_ERROR_UNKNOWN;draw_site=4;goto fail;}cursor+=n;
                n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
                if(!n){rc=VK_ERROR_UNKNOWN;draw_site=5;goto fail;}cursor+=n;
                cache(address,(size_t)bytes);
                n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)address,
                    bytes,recorded->clear_word);
                if(!n){rc=VK_ERROR_UNKNOWN;draw_site=6;goto fail;}cursor+=n;
                n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
                if(!n){rc=VK_ERROR_UNKNOWN;draw_site=7;goto fail;}cursor+=n;
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_D16_CLEAR_PREPARED serial=%llu word=%08x bytes=%llu",
                    (unsigned long long)j->serial,recorded->clear_word,
                    (unsigned long long)bytes);
                continue;
            }
            VkImageView view=recorded->framebuffer->attachments[
                ps5vk_render_pass_subpass(pass,subpass_index)->color[0].attachment];
            void *address;VkDeviceSize bytes,stride;
            rc=ps5vk_image_span(d,view->image,&address,&bytes);
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
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
            /* A multisampled attachment is cleared with one whole-surface fill:
             * the rect equation below addresses a single sample plane, while a
             * constant fill of the whole span writes every sample of every
             * texel whatever order the hardware stores them in - the same
             * operation the pass's own loadOp=CLEAR performs. The validator
             * admits this shape only when the clear IS the whole surface. */
            if(view->image->info.samples!=VK_SAMPLE_COUNT_1_BIT) {
                n=ps5vk_dma_fill(cursor,(size_t)(end-cursor),(uintptr_t)address,bytes,
                    recorded->clear_word);
                if(!n){rc=VK_ERROR_UNKNOWN;draw_site=6;goto fail;}cursor+=n;
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_MULTISAMPLE_CLEAR_PREPARED serial=%llu samples=%u bytes=%llu word=%08x",
                    (unsigned long long)j->serial,(unsigned)view->image->info.samples,
                    (unsigned long long)bytes,recorded->clear_word);
                n=ps5vk_graphics_acquire(cursor,(size_t)(end-cursor));
                if(!n){rc=VK_ERROR_UNKNOWN;draw_site=7;goto fail;}cursor+=n;
                continue;
            }
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
            /* A subpass that declares a resolve target has its resolved result
             * produced HERE, before the boundary carries the pass into the next
             * subpass: the draw is the driver's own (resolve_draw_emit). */
            rc=resolve_draw_emit(d,j,&cursor,&end,begin,subpass_index,defaults,&draw_site);
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
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
            BATCH_RESERVE(PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WAIT_WORDS+
                PS5VK_GRAPHICS_ACQUIRE_WORDS);
            const struct ps5vk_subpass *next_subpass=
                ps5vk_render_pass_subpass(pass,subpass_index);
            if(next_subpass->input_count || pass->dependency_count) {
                /* The transition that publishes colour to the texture path, and
                 * the token it waits for: the writeback is asynchronous, so an
                 * input-attachment read issued behind the bare event observed
                 * whatever the caches had not written back yet. */
                volatile uint64_t *token_slot=ps5vk_draw_batch_open_label(&j->chain);
                if(!token_slot){rc=VK_ERROR_UNKNOWN;draw_site=9;goto fail;}
                token_slot[7]=0;
                size_t color_barrier=ps5vk_graphics_color_to_texture_wait(
                    cursor,(size_t)(end-cursor),(uintptr_t)(token_slot+7),
                    ++j->barrier_tokens);
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
                if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
                if(!shaped && draw_has_work(&resolved)){shape=k;shaped=1;}
            }
            if(command_count)rc=ps5vk_indirect_resolve_command(d,recorded,shape,&resolved);
            else rc=ps5vk_indirect_resolve(d,recorded,&resolved);
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
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
                         * a null handle is encoded only as the zeroed
                         * nullDescriptor record, on a device that enabled it. */
                        if(!set->buffers[index].buffer && !(d->enabled_features_t09 & PS5VK_T09_FEATURE_NULL_DESCRIPTOR)) {
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
                        if(rc!=VK_SUCCESS){
                            ps5log_printf(PS5LOG_ERR,
                                "PS5VK_INPUT_ATTACHMENT_REFUSED serial=%llu subpass=%u inner=%u rc=%d "
                                "defined=%u layout=%u view_is_fb=%u ref=%u",
                                (unsigned long long)j->serial,subpass_index,
                                ps5vk_input_attachment_gate_site,(int)rc,
                                set?set->defined[index]:0u,
                                set?(unsigned)set->images[index].imageLayout:0u,
                                (unsigned)(set && set->images[index].imageView &&
                                    ps5vk_render_pass_subpass(op->render_pass,subpass_index) &&
                                    ps5vk_render_pass_subpass(op->render_pass,subpass_index)->input_count &&
                                    set->images[index].imageView == op->framebuffer->attachments[
                                        ps5vk_render_pass_inputs(op->render_pass,subpass_index)[0].attachment]),
                                ps5vk_render_pass_subpass(op->render_pass,subpass_index) &&
                                    ps5vk_render_pass_subpass(op->render_pass,subpass_index)->input_count ?
                                    ps5vk_render_pass_inputs(op->render_pass,subpass_index)[0].attachment : 0xffffffffu);
                            draw_site=__LINE__;goto fail;
                        }
                        continue;
                    }
                    /* nullDescriptor: a null sampled view has no image, so
                     * it carries no layout requirement. */
                    if(!set->images[index].imageView && !set->image_resources[index] &&
                       (d->enabled_features_t09 & PS5VK_T09_FEATURE_NULL_DESCRIPTOR))
                        continue;
                    if(!set->image_resources[index] ||
                       set->images[index].imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                        rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=18;goto fail;
                    }
                    VkImage sampled = set->image_resources[index];
                    if (sampled->subresource_layouts) {
                        VkImageView view = set->images[index].imageView;
                        rc = view && view->image == sampled &&
                            ps5vk_image_range_layout_matches(sampled, &view->range,
                                set->images[index].imageLayout) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
                    } else rc=ps5vk_layout_require(&j->layouts,sampled,set->images[index].imageLayout);
                    if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
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
            rc=ps5vk_index_fetch_prepare(d,op,&indices);if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
        }
        const uint32_t vertex_usage=p->pair->runtime_arguments.enabled?
            (p->pair->runtime_arguments.vertex_buffer_usage_mask |
             p->pair->hull_arguments.vertex_buffer_usage_mask):
            (op->pipeline->vertex_binding_count?1u:0u);
        const struct ps5vk_graphics_key key={.vertex_binding_count=op->pipeline->vertex_binding_count,
            .vertex_attribute_count=op->pipeline->vertex_attribute_count,
            .vertex_bindings=op->pipeline->vertex_bindings,.vertex_attributes=op->pipeline->vertex_attributes};
        /* The targets are the CURRENT SUBPASS's own references, not the
         * framebuffer's role lists: those are subpass 0's roles, and a pass
         * whose later subpass renders elsewhere would otherwise program the
         * wrong surface. For every pass this executor served before the pinned
         * multisample oracle the two are the same list. */
        struct ps5vk_target_set targets;
        rc=ps5vk_target_set_from_subpass(pass,op->framebuffer,subpass_index,&targets);
        if(rc!=VK_SUCCESS){draw_site=32;goto fail;}
        if(vertex_usage) {
            rc=ps5vk_native_prepare_vertex_draw_masked(d,op,&begin->render_area,defaults,&key,
                (uintptr_t)p->pair,vertex_usage,&targets,draw);
        } else rc=ps5vk_native_prepare_resource_draw(d,op,&begin->render_area,defaults,(uintptr_t)p->pair,&targets,draw);
        if(rc!=VK_SUCCESS) {
            ps5log_printf(PS5LOG_ERR,"PS5VK_DRAW_RESOURCE_PREPARE_FAILED serial=%llu vertex_mask=%x bindings=%u attributes=%u site=%u state_site=%u rc=%d",
                (unsigned long long)j->serial,vertex_usage,key.vertex_binding_count,
                key.vertex_attribute_count,ps5vk_draw_prepare_site,ps5vk_draw_state_site,(int)rc);
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
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
            VkFramebuffer fb=op->framebuffer;
            /* The target the draw was prepared with is layer zero of the same
             * builder, so the emission can carry only what a layer moves. */
            rc=ps5vk_native_layer_target(d,fb->attachments[fb->color_attachments[0]],0u,
                defaults,&view_prepared_color);
            if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
            if(depth) {
                rc=ps5vk_native_layer_target(d,fb->attachments[fb->depth_attachment],0u,
                    NULL,&view_prepared_depth);
                if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
            }
            for(uint32_t v=0;v<view_count;++v) {
                rc=ps5vk_native_view_layer_target(d,fb->attachments[fb->color_attachments[0]],
                    view_indices[v],defaults,&view_color[v]);
                if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
                if(depth) {
                    rc=ps5vk_native_view_layer_target(d,fb->attachments[fb->depth_attachment],
                        view_indices[v],NULL,&view_depth[v]);
                    if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
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
                if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
                op=&resolved;
                /* DrawIndex k is consumed whether or not the command draws. */
                if(!draw_has_work(op))continue;
                if(op->type==PS5VK_DRAW_INDEXED) {
                    rc=ps5vk_index_fetch_prepare(d,op,&indices);if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
                }
                /* The shared vertex table spans the whole bound buffers; this
                 * command's own firstVertex+vertexCount must still lie inside
                 * them, judged by the same code that judged the shaping one. */
                if(vertex_usage) {
                    struct ps5vk_vertex_fetch_table check;
                    rc=ps5vk_vertex_fetch_used_spans(d,&key,op,vertex_usage,&check);
                    if(rc!=VK_SUCCESS){draw_site=__LINE__;goto fail;}
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
                       emission==(uint32_t *)j->chain.arenas[j->chain.count-1].address+PS5VK_GRAPHICS_ACQUIRE_WORDS){draw_site=__LINE__;goto fail;}
                    j->chain.cursor=cursor;
                    VkResult retry_rc=ps5vk_draw_batch_retry(&j->chain,&emission);
                    if(retry_rc!=VK_SUCCESS){rc=retry_rc;draw_site=__LINE__;goto fail;}
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
    if(active_query_slot>=0) {rc=VK_ERROR_FEATURE_NOT_PRESENT;draw_site=27;goto fail;}
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15
    if(j->slot_active) {
        size_t n=ps5vk_graphics_occlusion_event(cursor,(size_t)(end-cursor),
            (uint64_t)(uintptr_t)j->slot.address+8);
        if(!n){rc=VK_ERROR_UNKNOWN;draw_site=25;goto fail;}cursor+=n;
#if defined(PS5VK_OCCLUSION_PRECISE_PROBE) && PS5VK_OCCLUSION_PRECISE_PROBE
        size_t control=ps5vk_graphics_occlusion_control(cursor,(size_t)(end-cursor),0);
        if(!control){rc=VK_ERROR_UNKNOWN;draw_site=25;goto fail;}cursor+=control;
#endif
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_OCCLUSION_PROBE_END serial=%llu base_plus_8=%llx",
            (unsigned long long)j->serial,(unsigned long long)(uintptr_t)j->slot.address+8);
    }
#endif
    /* The last subpass ends with the pass: its resolve target, if it declares
     * one, is produced before the postlude acts on the pass's result. */
    if(pass->subpass_count) {
        rc=resolve_draw_emit(d,j,&cursor,&end,begin,pass->subpass_count-1u,defaults,&draw_site);
        if(rc!=VK_SUCCESS)goto fail;
    }
    PHASE("postlude");
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
            if(rc!=VK_SUCCESS && partition.prefix_count) {
                for(unsigned k=0;k<partition.prefix_count;++k) {
                    const struct ps5vk_operation *op=&postlude[k];
                    if(op->type==PS5VK_IMAGE_BARRIER) {
                        VkImage image=op->image_barrier.image;
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u stages=%08x/%08x access=%08x/%08x layout=%u/%u format=%u usage=%08x",
                            k,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                            (unsigned)op->src_access,(unsigned)op->dst_access,
                            (unsigned)op->image_barrier.oldLayout,
                            (unsigned)op->image_barrier.newLayout,
                            image?(unsigned)image->info.format:0u,
                            image?(unsigned)image->info.usage:0u);
                    } else if(op->type==PS5VK_COPY_BUFFER_IMAGE) {
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u layout=%u format=%u usage=%08x offset=%llu extent=%ux%ux%u row=%u image_height=%u",
                            k,(unsigned)op->type,(unsigned)op->copy_layout,
                            op->copy_image?(unsigned)op->copy_image->info.format:0u,
                            op->copy_image?(unsigned)op->copy_image->info.usage:0u,
                            (unsigned long long)op->copy_region.bufferOffset,
                            op->copy_region.imageExtent.width,op->copy_region.imageExtent.height,
                            op->copy_region.imageExtent.depth,op->copy_region.bufferRowLength,
                            op->copy_region.bufferImageHeight);
                    } else {
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u stages=%08x/%08x access=%08x/%08x",
                            k,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                            (unsigned)op->src_access,(unsigned)op->dst_access);
                    }
                }
            }
            if(rc==VK_SUCCESS)
                rc=ps5vk_readback_commands(d,postlude+partition.readback_first,
                    partition.readback_count,j->color,&j->layouts,&plan,&draw_site);
            if(rc==VK_SUCCESS && partition.suffix_count)
                rc=ps5vk_upload_commands(d,postlude+partition.suffix_first,
                    partition.suffix_count,j->color,&j->layouts,&cursor,end,cache);
            if(rc!=VK_SUCCESS) {
                ps5log_printf(PS5LOG_INFO,
                    "PS5VK_GRAPHICS_POSTLUDE_SPLIT total=%u prefix=%u readback_first=%u readback_count=%u readback=%u",
                    postlude_count,partition.prefix_count,partition.readback_first,
                    partition.readback_count,readback);
                for(unsigned k=0;k<postlude_count;++k) {
                    const struct ps5vk_operation *op=&postlude[k];
                    if(op->type==PS5VK_IMAGE_BARRIER) {
                        VkImage image=op->image_barrier.image;
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u stages=%08x/%08x access=%08x/%08x layout=%u/%u format=%u usage=%08x",
                            k,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                            (unsigned)op->src_access,(unsigned)op->dst_access,
                            (unsigned)op->image_barrier.oldLayout,
                            (unsigned)op->image_barrier.newLayout,
                            image?(unsigned)image->info.format:0u,
                            image?(unsigned)image->info.usage:0u);
                    } else if(op->type==PS5VK_COPY_IMAGE_BUFFER) {
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u buffer=%u format=%u usage=%08x layout=%u extent=%ux%ux%u row=%u image_height=%u",
                            k,(unsigned)op->type,op->copy_destination?1u:0u,
                            op->copy_image?(unsigned)op->copy_image->info.format:0u,
                            op->copy_image?(unsigned)op->copy_image->info.usage:0u,
                            (unsigned)op->copy_layout,op->copy_region.imageExtent.width,
                            op->copy_region.imageExtent.height,op->copy_region.imageExtent.depth,
                            op->copy_region.bufferRowLength,op->copy_region.bufferImageHeight);
                    } else if(op->type==PS5VK_COPY_BUFFER_IMAGE) {
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u format=%u usage=%08x layout=%u extent=%ux%ux%u row=%u image_height=%u",
                            k,(unsigned)op->type,
                            op->copy_image?(unsigned)op->copy_image->info.format:0u,
                            op->copy_image?(unsigned)op->copy_image->info.usage:0u,
                            (unsigned)op->copy_layout,op->copy_region.imageExtent.width,
                            op->copy_region.imageExtent.height,op->copy_region.imageExtent.depth,
                            op->copy_region.bufferRowLength,op->copy_region.bufferImageHeight);
                    } else {
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u stages=%08x/%08x access=%08x/%08x",
                            k,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                            (unsigned)op->src_access,(unsigned)op->dst_access);
                    }
                }
            }
            if(rc!=VK_SUCCESS)goto fail;
            j->readback.count=1u;j->readback.target[0]=plan;
        } else {
            rc=ps5vk_upload_commands(d,postlude,postlude_count,j->color,
                &j->layouts,&cursor,end,cache);
            if(rc!=VK_SUCCESS) {
                for(unsigned k=0;k<postlude_count;++k) {
                    const struct ps5vk_operation *op=&postlude[k];
                    if(op->type==PS5VK_IMAGE_BARRIER) {
                        VkImage image=op->image_barrier.image;
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u stages=%08x/%08x access=%08x/%08x layout=%u/%u format=%u usage=%08x",
                            k,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                            (unsigned)op->src_access,(unsigned)op->dst_access,
                            (unsigned)op->image_barrier.oldLayout,
                            (unsigned)op->image_barrier.newLayout,
                            image?(unsigned)image->info.format:0u,
                            image?(unsigned)image->info.usage:0u);
                    } else if(op->type==PS5VK_COPY_BUFFER_IMAGE) {
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u layout=%u format=%u usage=%08x offset=%llu extent=%ux%ux%u row=%u image_height=%u",
                            k,(unsigned)op->type,(unsigned)op->copy_layout,
                            op->copy_image?(unsigned)op->copy_image->info.format:0u,
                            op->copy_image?(unsigned)op->copy_image->info.usage:0u,
                            (unsigned long long)op->copy_region.bufferOffset,
                            op->copy_region.imageExtent.width,op->copy_region.imageExtent.height,
                            op->copy_region.imageExtent.depth,op->copy_region.bufferRowLength,
                            op->copy_region.bufferImageHeight);
                    } else {
                        ps5log_printf(PS5LOG_INFO,
                            "PS5VK_GRAPHICS_POSTLUDE_OP index=%u type=%u stages=%08x/%08x access=%08x/%08x",
                            k,(unsigned)op->type,(unsigned)op->src_stage,(unsigned)op->dst_stage,
                            (unsigned)op->src_access,(unsigned)op->dst_access);
                    }
                }
            }
            if(rc!=VK_SUCCESS)goto fail;
        }
    }
    /* The segments the submission recorded AFTER the pass. The pinned module's
     * readback reads every attachment its pass rendered back into its own
     * buffer and publishes those copies; the executor stages the plan here,
     * because the bytes become readable over the CPU detile that runs after
     * this submission's exact completion label. Any other trailing segment is
     * transfer work the same emitter already owns. */
    phase="postlude-segments";
    for(unsigned i=next_buffer;i<s->count;++i) {
        VkCommandBuffer listed=s->buffers[i];
        const uint32_t listed_first=ps5vk_submission_first_operation(s,i);
        const uint32_t listed_count=ps5vk_submission_operation_count(s,i);
        unsigned readback=0;
        for(uint32_t k=listed_first;k<listed_first+listed_count;++k)
            readback|=listed->operations[k].type==PS5VK_COPY_IMAGE_BUFFER;
        j->chain.cursor=cursor;
        if(readback) {
            struct ps5vk_readback_set set={0};
            rc=ps5vk_readback_commands_set(d,listed->operations+listed_first,listed_count,
                NULL,&j->layouts,&set);
            if(rc!=VK_SUCCESS)goto fail;
            j->readback=set;
        } else {
            rc=ps5vk_upload_commands(d,listed->operations+listed_first,listed_count,NULL,
                &j->layouts,&cursor,end,cache);
            if(rc!=VK_SUCCESS)goto fail;
        }
    }
    j->chain.cursor=cursor;
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE && PS5VK_GRAPHICS_SCISSOR_PROBE!=15
    _Static_assert(8+PS5VK_GRAPHICS_PROBE_REGISTERS*4<=64,"probe must not overlap command words or leave reserved tail");
    BATCH_RESERVE(PS5VK_GRAPHICS_PROBE_WORDS+16u);
    uint32_t *probe=(uint32_t *)(ps5vk_draw_batch_open_label(&j->chain)+1);
    for(unsigned i=0;i<PS5VK_GRAPHICS_PROBE_REGISTERS;++i)probe[i]=0xd15ea5e0u+i;
    size_t probe_words=ps5vk_graphics_register_probe(cursor,(size_t)(end-cursor),(uintptr_t)probe);
    if(!probe_words){rc=VK_ERROR_UNKNOWN;goto fail;}cursor+=probe_words;
#endif
    PHASE("release-packet");
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
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE==15 && \
    defined(PS5VK_OCCLUSION_PRECISE_PROBE) && PS5VK_OCCLUSION_PRECISE_PROBE
    /* Dump the complete emitted PM4 stream for the one precise-counter
     * diagnostic. This makes it possible to see whether later draw-state
     * packets overwrite DB_COUNT_CONTROL after our explicit setting. */
    if(j->slot_active) {
        for(unsigned b=0;b<j->chain.count;++b) {
            const uint32_t *w=(const uint32_t *)j->chain.arenas[b].address;
            const uint32_t n=j->chain.words[b];
            for(uint32_t i=0;i<n;i+=4)
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_OCCLUSION_PM4 serial=%llu arena=%u at=%u of=%u "
                    "%08x %08x %08x %08x",
                    (unsigned long long)j->serial,b,i,n,
                    w[i],i+1<n?w[i+1]:0u,i+2<n?w[i+2]:0u,
                    i+3<n?w[i+3]:0u);
        }
    }
#endif
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
        "PS5VK_GRAPHICS_PREPARE_FAILED serial=%llu phase=%s site=%u early=%u rc=%d",
        j?(unsigned long long)j->serial:0ull,phase,draw_site,site_report,rc);
    if(j)release(d,j);
    return rc;
}
#undef PHASE
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
        for(unsigned r=0;r<j->readback.count;++r) {
            void *source;VkDeviceSize source_bytes;
            void *destination;VkDeviceSize destination_bytes;
            VkImage image=j->readback.target[r].image;
            if(!image || ps5vk_image_span(d,image,&source,&source_bytes)!=VK_SUCCESS ||
               ps5vk_buffer_span(d,j->readback.target[r].buffer,0,VK_WHOLE_SIZE,
                    &destination,&destination_bytes)!=VK_SUCCESS ||
               (j->readback.target[r].aspect ?
                ps5vk_depth_stencil_readback_detile(image,j->readback.target[r].aspect,
                    destination,(size_t)destination_bytes,source,source_bytes) :
                ps5vk_readback_detile(image,(size_t)j->readback.target[r].layer_stride,
                    destination,(size_t)destination_bytes,source,source_bytes)))
                return VK_ERROR_DEVICE_LOST;
            /* This bounded implementation performs the transfer-copy result
             * publication on the CPU only after exact GPU completion. The
             * allocation remains non-coherent; the application still calls
             * vkInvalidateMappedMemoryRanges before host reads. */
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_GRAPHICS_READBACK serial=%llu target=%u width=%u height=%u bytes=%llu "
                "mode=cpu-detile-after-gpu",
                (unsigned long long)j->serial,r,image->info.extent.width,image->info.extent.height,
                (unsigned long long)destination_bytes);
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 4
            /* Private diagnostic only. Never change pixels or upstream's
             * comparator; count just the exact single-layer packed image. */
            if((image->info.format==VK_FORMAT_R8G8B8A8_UNORM ||
                image->info.format==VK_FORMAT_B8G8R8A8_UNORM ||
                image->info.format==VK_FORMAT_R8G8B8A8_UINT ||
                image->info.format==VK_FORMAT_R8G8B8A8_SINT) &&
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
        if(j->query_arena_active) {
            for(unsigned q=0;q<j->query_count;++q) {
                uint64_t *pair=j->queries[q].counters;
                cache(pair,PS5VK_QUERY_SLOT_BYTES);
                uint64_t sum=0;
                unsigned available=0;
                for(unsigned rb=0;rb<PS5VK_QUERY_COUNTER_PAIRS;++rb) {
                    uint64_t begin=pair[2u*rb],end=pair[2u*rb+1u];
                    VkBool32 begin_valid=(begin>>63)!=0;
                    VkBool32 end_valid=(end>>63)!=0;
                    if(begin_valid!=end_valid)return VK_ERROR_DEVICE_LOST;
                    if(!begin_valid)continue;
                    begin&=~(UINT64_C(1)<<63);
                    end&=~(UINT64_C(1)<<63);
                    if(end<begin || UINT64_MAX-sum<end-begin)
                        return VK_ERROR_DEVICE_LOST;
                    sum+=end-begin;
                    ++available;
                }
                if(!available || ps5vk_query_publish(d,j->queries[q].pool,
                        j->queries[q].query,sum)!=VK_SUCCESS)
                    return VK_ERROR_DEVICE_LOST;
                ps5log_printf(PS5LOG_MARK,
                    "PS5VK_OCCLUSION_QUERY serial=%llu query=%u passed_samples=%llu render_backends=%u available=1 precise=1",
                    (unsigned long long)j->serial,j->queries[q].query,
                    (unsigned long long)sum,available);
            }
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

/*
 * Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The fragment descriptor-table bridge follows the bounded PSBC/Gallium ABI
 * documented by the pinned ps5-opengl reference; packet validation and
 * Vulkan-facing ownership are ps5-vulkan work.
 */
#include "draw_emit_ps5.h"
#include "draw_parameters.h"
#include "ps5_agc_writer.h"
#include "ps5_gpu_span.h"
#include <string.h>

/* Public gfx10 SET_CONTEXT_REG packet, the same shape the scissor replay below
 * already emits: a type-3 header whose payload length is the number of
 * registers, the base register offset, then one word per register. Only
 * consecutive offsets can share a packet, so a sparse register list becomes one
 * packet per run. */
enum { PS5VK_SET_CONTEXT_REG = 0xc0006900 };

/* The registers the draw state writes AFTER the target block: this profile's
 * depth control, clip and cull policy, scissor and rasterization precision. A
 * view changes where an attachment's layers live, never how fragments are
 * decided, so a view that MOVES one of them is refused rather than applied over
 * the pipeline state. Carrying the builder's own immutable value for one is not
 * a move: the D32 target always contains offset 0x200 with its constant
 * DB_RENDER_CONTROL value, and refusing that word would make every multiview
 * depth draw unrepresentable. */
static int pipeline_owned_register(uint32_t offset)
{
    switch (offset) {
    case 0x094: case 0x095: case 0x200: case 0x204: case 0x205:
    case 0x206: case 0x292: case 0x2f9: return 1;
    }
    return 0;
}

/* The dwords a run-grouped emission of this register list takes: one packet per
 * maximal run of consecutive offsets, each packet a header, the base offset and
 * one word per register. Sparse lists become one packet per register. */
static uint32_t context_run_words(const ps5_agc_register *registers, uint32_t count)
{
    uint32_t words = 0, emitted = 0;
    while (emitted < count) {
        uint32_t run = 1;
        while (emitted + run < count &&
               registers[emitted + run].offset == registers[emitted].offset + run) ++run;
        words += run + 2u;
        emitted += run;
    }
    return words;
}

static VkResult emit_context_runs(uint32_t **cursor, uint32_t capacity,
    const ps5_agc_register *registers, uint32_t count)
{
    if (!cursor || !*cursor || !registers || !count) return VK_ERROR_UNKNOWN;
    if (context_run_words(registers, count) > capacity) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *next = *cursor;
    uint32_t emitted = 0;
    while (emitted < count) {
        uint32_t run = 1;
        while (emitted + run < count &&
               registers[emitted + run].offset == registers[emitted].offset + run) ++run;
        *next++ = (uint32_t)PS5VK_SET_CONTEXT_REG | (run << 16);
        *next++ = registers[emitted].offset;
        for (uint32_t k = 0; k < run; ++k) *next++ = registers[emitted + k].value;
        emitted += run;
    }
    *cursor = next;
    return VK_SUCCESS;
}

/* Append the words a view's layer moves relative to the target the draw was
 * prepared with. Both lists are the same builder's output for the same image,
 * so each list has to BE that builder's exact shape and a view may only differ
 * where the layer address is carried (ps5vk_target_offsets and
 * ps5vk_target_carrier): a list of another length, an offset that is not the
 * builder's at that position, a changed word that does not carry the address,
 * or a changed word the pipeline state owns are all refused. Nothing is written
 * here - the caller emits only once every list of the whole re-emission has been
 * accepted. */
static VkResult collect_view_layer(ps5_agc_register *changed, uint32_t *changed_count,
    uint32_t capacity, const struct ps5vk_target_registers *prepared,
    const struct ps5vk_target_registers *view)
{
    if (!changed || !changed_count || !prepared || !view || !view->count ||
        prepared->count != view->count || *changed_count > capacity)
        return VK_ERROR_UNKNOWN;
    const uint32_t *offsets = ps5vk_target_offsets(view->count);
    if (!offsets || view->count > capacity - *changed_count) return VK_ERROR_UNKNOWN;
    for (uint32_t i = 0; i < view->count; ++i) {
        if (prepared->registers[i].offset != offsets[i] ||
            view->registers[i].offset != offsets[i]) return VK_ERROR_UNKNOWN;
        if (prepared->registers[i].value == view->registers[i].value) continue;
        if (!ps5vk_target_carrier(offsets[i])) return VK_ERROR_UNKNOWN;
        if (pipeline_owned_register(offsets[i])) return VK_ERROR_UNKNOWN;
        changed[(*changed_count)++] = view->registers[i];
    }
    return VK_SUCCESS;
}

/* The per-view layer selection of one re-emitted draw, assembled in full -
 * including the dwords its packets will take - before the emission writes its
 * first word, so every semantic view error is a refusal that leaves the target
 * buffer untouched rather than merely unadvanced. A view whose layer is already
 * the prepared target contributes no word at all, which is what keeps the
 * multiview-disabled emission byte-identical. */
static VkResult collect_view_targets(ps5_agc_register *changed,uint32_t *changed_count,
    uint32_t *changed_words,uint32_t capacity,const struct ps5vk_view_emit *view)
{
    if (!changed || !changed_count || !changed_words || !view ||
        view->view_index >= PS5VK_MAX_VIEW_MASK_VIEWS ||
        !view->prepared_color || !view->view_color ||
        (!!view->prepared_depth != !!view->view_depth)) return VK_ERROR_UNKNOWN;
    VkResult rc = collect_view_layer(changed, changed_count, capacity,
        view->prepared_color, view->view_color);
    if (rc == VK_SUCCESS && view->prepared_depth)
        rc = collect_view_layer(changed, changed_count, capacity,
            view->prepared_depth, view->view_depth);
    if (rc != VK_SUCCESS) return rc;
    *changed_words = context_run_words(changed, *changed_count);
    return VK_SUCCESS;
}

VkResult ps5vk_native_emit_scissor_replay(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state)
{
    if(!cursor || !*cursor || !state || state->cx_count>PS5VK_DRAW_CX_CAPACITY)return VK_ERROR_UNKNOWN;
    uint32_t tl=0,br=0,mode=0,found=0;
    for(unsigned i=0;i<state->cx_count;++i) {
        switch(state->cx[i].offset) {
        case 0x094:tl=state->cx[i].value;found|=1;break;
        case 0x095:br=state->cx[i].value;found|=2;break;
        case 0x292:mode=state->cx[i].value;found|=4;break;
        }
    }
    if(found!=7)return VK_ERROR_UNKNOWN;
    if(capacity<7)return VK_ERROR_OUT_OF_HOST_MEMORY;
    const uint32_t words[7]={0xc0026900,0x094,tl,br,0xc0016900,0x292,mode};
    memcpy(*cursor,words,sizeof(words));*cursor+=7;return VK_SUCCESS;
}

static VkResult emit_draw(uint32_t **cursor, uint32_t capacity,
    const struct ps5vk_draw_state *state, const void *mapping, size_t mapping_bytes,
    const struct ps5vk_operation *op, uint32_t global_table_low,
    uint32_t vertex_table_low, int vertex_input,
    const struct ps5vk_index_fetch *indices,ps5vk_emit_index_fn emit_index,const uint32_t *texture_low,
    const uint32_t *descriptor_tables,const struct ps5vk_view_emit *view)
{
    if (!cursor || !*cursor || !state || !op || op->type != (indices?PS5VK_DRAW_INDEXED:PS5VK_DRAW) ||
        !state->cx_count || state->cx_count > PS5VK_DRAW_CX_CAPACITY || !state->modifier ||
        !ps5_gpu_span_visible(mapping, mapping_bytes, state, sizeof(*state)) ||
        (view && !state->runtime.enabled)) return VK_ERROR_UNKNOWN;
    /* Vulkan zero-count draws have no rasterization side effects. */
    if (!(indices?op->index_count:op->vertex_count) || !op->instance_count) return VK_SUCCESS;
    uint32_t runtime_vertex[16],runtime_pixel[16];
    uint32_t sh_count=state->sh_count?state->sh_count:12;
    if(sh_count>PS5VK_DRAW_SH_CAPACITY)return VK_ERROR_UNKNOWN;
    const uint32_t single_table[PS5VK_RUNTIME_DESCRIPTOR_SETS]={texture_low?*texture_low:0,0,0,0};
    /* The runtime ABI describes how the compiled stages receive their user
     * SGPRs; it is orthogonal to how vertices are addressed. An indexed draw
     * runs through the same prepared vertex table and the same
     * ps5vk_native_emit_index emitter as the offline path, with only the
     * shader-visible base vertex changing value: the signed vertexOffset
     * (draw_parameters.h). The emitter keeps its own fetch checks and the
     * caller's cursor still advances only on full success, so an unsupported
     * shape stays fail-closed instead of reaching the queue. */
    if(state->runtime.enabled &&
        ps5vk_runtime_draw_values_sets(&state->runtime,ps5vk_draw_base_vertex(op),
            ps5vk_draw_base_instance(op),ps5vk_draw_index_value(op),
            view?view->view_index:ps5vk_draw_view_index_value(op),
            vertex_input?vertex_table_low:0,state->push_constant_low,
            descriptor_tables?descriptor_tables:single_table,
            runtime_vertex,runtime_pixel))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    ps5_agc_register view_changed[PS5_COLOR_REGISTER_COUNT + PS5_DEPTH_REGISTER_COUNT];
    uint32_t view_changed_count=0,view_words=0;
    if(view) {
        VkResult view_rc=collect_view_targets(view_changed,&view_changed_count,&view_words,
            PS5_COLOR_REGISTER_COUNT+PS5_DEPTH_REGISTER_COUNT,view);
        if(view_rc!=VK_SUCCESS)return view_rc;
    }
    /* The view's footprint is preflighted here, before the first word is
     * written, so the only failures left after this point are this emitter's
     * historic ones (a capacity or callback failure inside the emitters below).
     * Those may leave bytes in this unsubmitted scratch - the documented
     * contract is that the caller's cursor does not advance and the whole job is
     * discarded - so no stronger promise is made or needed here. */
    if (capacity < 17u + view_words) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *next = *cursor, *end = next + capacity;
#if defined(PS5VK_TESS_VGT_FLUSH) && PS5VK_TESS_VGT_FLUSH
    /* WAIT FOR IDLE AND RESET THE VGT's POINTERS BEFORE UPDATING ITS RING
     * STATE, which this driver has never done.
     *
     * A patch draw rewrites VGT_TF_RING_SIZE, VGT_HS_OFFCHIP_PARAM,
     * VGT_TF_MEMORY_BASE and its high word on EVERY draw, because the
     * tessellation rings belong to the pipeline rather than to a device
     * initialisation this driver does not perform.
     *
     * THE CITATION IS NARROWER THAN IT FIRST LOOKS, and the distinction
     * matters: the sequence below is quoted verbatim from ac_shadowed_regs.c,
     * but it lives in ac_build_load_reg(), which builds the REGISTER-SHADOW
     * LOAD preamble - not a general per-draw guard for updating ring
     * registers. The comments are the pinned tree's own and they describe
     * what the events do; they do not establish that a driver must emit them
     * before every ring update. This is therefore a diagnostic, default off,
     * and it stays one. In the pinned tree's words:
     *
     *   "Wait for idle, because we'll update VGT ring pointers."
     *     EVENT_WRITE(VS_PARTIAL_FLUSH, EVENT_INDEX(4))
     *   "VGT_FLUSH is required even if VGT is idle. It resets VGT pointers."
     *     EVENT_WRITE(VGT_FLUSH, EVENT_INDEX(0))
     *
     * Our command stream contains no events at all - the dump of a whole
     * patch submission is three indirect register packets, two direct shader
     * writes, NUM_INSTANCES and the draw. So the geometry engine is told
     * where the tessellation factor ring lives without ever being made to
     * re-read it, and a stale internal pointer would explain the one result
     * nothing else does: pre-filling this driver's entire factor ring with a
     * legal level changed nothing, which is what you would expect if the
     * engine is reading a different ring altogether.
     *
     * Emitted before the register banks, in the pinned order: idle first,
     * then the pointer reset, then the new values. */
    if (end - next < 4) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *next++ = 0xc0004600u;  /* PKT3(PKT3_EVENT_WRITE, 0, 0) */
    *next++ = 0x0000040fu;  /* VS_PARTIAL_FLUSH, EVENT_INDEX(4) */
    *next++ = 0xc0004600u;
    *next++ = 0x00000024u;  /* VGT_FLUSH, EVENT_INDEX(0) */
#endif
    if (ps5_agc_writer_set_indirect(&next, (uint32_t)(end-next), state->cx, state->cx_count,
            mapping, mapping_bytes, sceAgcDcbSetCxRegistersIndirect) ||
        ps5_agc_writer_set_indirect(&next, (uint32_t)(end-next), state->uc, state->uc_count,
            mapping, mapping_bytes, sceAgcDcbSetUcRegistersIndirect) ||
        ps5_agc_writer_set_indirect(&next, (uint32_t)(end-next), state->sh, sh_count,
            mapping, mapping_bytes, sceAgcDcbSetShRegistersIndirect)) return VK_ERROR_UNKNOWN;
#if defined(PS5VK_TESS_PROBE) && PS5VK_TESS_PROBE
    /* Read the GPU register file, rather than the CPU's intended bank.
     * COPY_DATA register source -> TC_L2 memory, with write confirmation.
     * The upper half of the diagnostic ring table is unused by its two SRDs. */
    if (state->runtime.ring_table_valid) {
        if (end-next < 12) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i=0;i<state->uc_count;++i) {
            const uint32_t reg=state->uc[i].offset;
            if (reg==0x24e || reg==0x24f || reg==0x250 || reg==0x261) {
                *next++=0xc0017900u;
                *next++=reg;
                *next++=state->uc[i].value;
            }
        }
        const uint32_t regs[] = {0x30938,0x3093c,0x30940,0x30984,
            0x28b54,0x28b58,0x28b6c,0x3096c,
            0x28a18,0x28a1c,0x30908,0x30980,
            0xb320,0xb228,0xb22c,0xb520};
        const uint64_t table = ((uint64_t)state->runtime.ring_table_high<<32) |
            state->runtime.ring_table_low;
        if (end-next < 96) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (unsigned i=0;i<16;++i) {
            const uint64_t dst=table+128+4*i;
            *next++=0xc0044000u;
            *next++=(2u<<8)|(1u<<20);
            *next++=regs[i]>>2;
            *next++=0;
            *next++=(uint32_t)dst;
            *next++=(uint32_t)(dst>>32);
        }
    }
#endif
#if defined(PS5VK_TESS_DIRECT_INDEXED) && PS5VK_TESS_DIRECT_INDEXED
    /* DIAGNOSTIC, default off: write the two INDEXED registers of a patch
     * draw as individual SET packets after the bulk loads.
     *
     * PAL's CmdUtil::IsIndexedRegister names VGT_LS_HS_CONFIG and
     * VGT_PRIMITIVE_TYPE (with VGT_INDEX_TYPE, VGT_NUM_INSTANCES and the
     * RSRC3/RSRC4 pairs) as registers the CP handles specially, and
     * CmdStream::WriteRegisters says "indexed registers must be written
     * individually with no other registers" - PAL never puts them in a
     * multi-register sequence and never reaches them through a LOAD. This
     * driver reaches both ONLY through LOAD_*_REG_INDEX pair-mode loads of
     * 100 and 10 registers. The GPU-side readback after those loads returns
     * the intended value for GE_CNTL but ZERO for VGT_PRIMITIVE_TYPE, so the
     * bulk load demonstrably reaches ordinary uconfig state and demonstrably
     * does not show up for this one.
     *
     * The mechanism fits everything measured: the LS/HS stage enables launch
     * the hull regardless, and it stores its factors; if the engine still
     * assembles TRIANGLES instead of PATCHES, or has a zero patch count in
     * its own copy of LS_HS_CONFIG, there is nothing for the tessellator to
     * tessellate and the evaluation half never launches, with no fault.
     *
     * Mode 1 is PAL's gfx10 form: plain SET_CONTEXT_REG / SET_UCONFIG_REG,
     * count 1 - PAL notes gfx10 dropped the index field for
     * VGT_PRIMITIVE_TYPE. Mode 2 is Mesa's form on every GFX7+ part:
     * SET_UCONFIG_REG_INDEX with index 1 for the primitive type and the
     * context write with index 2 in the offset dword for LS_HS_CONFIG.
     *
     * MEASURED AND CLOSED, both modes: the packets reached the stream in the
     * intended encodings (confirmed in the command-word dump), the draw
     * completed, and the evaluation half still produced nothing. Also
     * learned: VGT_PRIMITIVE_TYPE reads back zero through COPY_DATA even
     * after a direct SET that provably landed, so it is not readable that
     * way and its zero readback was never evidence of a missed load - the
     * readback argument above is withdrawn; the PAL rule stood alone and
     * failed too. Stays default off. */
    if (state->runtime.ring_table_valid) {
        uint32_t ls_hs=0,prim=0; int have_ls_hs=0,have_prim=0;
        for (uint32_t i=0;i<state->cx_count;++i)
            if (state->cx[i].offset==0x2d6) { ls_hs=state->cx[i].value; have_ls_hs=1; }
        for (uint32_t i=0;i<state->uc_count;++i)
            if (state->uc[i].offset==0x242) { prim=state->uc[i].value; have_prim=1; }
        if (end-next < 6) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (have_prim) {
#if PS5VK_TESS_DIRECT_INDEXED==2
            *next++=0xc0017a00u;              /* SET_UCONFIG_REG_INDEX */
            *next++=0x242u|(1u<<28);          /* index 1: primitive type */
#else
            *next++=0xc0017900u;              /* SET_UCONFIG_REG */
            *next++=0x242u;
#endif
            *next++=prim;
        }
        if (have_ls_hs) {
            *next++=0xc0016900u;              /* SET_CONTEXT_REG */
#if PS5VK_TESS_DIRECT_INDEXED==2
            *next++=0x2d6u|(2u<<28);          /* index 2: LS_HS_CONFIG */
#else
            *next++=0x2d6u;
#endif
            *next++=ls_hs;
        }
    }
#endif
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE
    VkResult scissor_rc=ps5vk_native_emit_scissor_replay(&next,(uint32_t)(end-next),state);
    if(scissor_rc!=VK_SUCCESS)return scissor_rc;
#endif
    /* The view's own layer selection lands after the prepared target block and
     * before anything that follows it, so the draw packet that follows renders
     * into this view's layer. */
    if(view_changed_count) {
        VkResult view_rc=emit_context_runs(&next,(uint32_t)(end-next),
            view_changed,view_changed_count);
        if(view_rc!=VK_SUCCESS)return view_rc;
    }
    const uint32_t procedural[3] = {global_table_low, ps5vk_draw_base_vertex(op),
        ps5vk_draw_base_instance(op)};
    const uint32_t vertex[4] = {global_table_low, vertex_table_low,
        ps5vk_draw_base_vertex(op), ps5vk_draw_base_instance(op)};
    const uint32_t fragment[2]={global_table_low,texture_low?*texture_low:0};
    if(state->runtime.enabled) {
#if defined(PS5VK_TESS_LEGACY_DOMAIN) && PS5VK_TESS_LEGACY_DOMAIN
        /* DIAGNOSTIC: a legacy hardware VS loads its user data from
         * SPI_SHADER_USER_DATA_VS_0 (sh 0x4c) with no system preamble, so the
         * tessellation pipeline's pre-raster block is written there too. */
        if(state->runtime.ring_table_valid &&
           ps5_agc_writer_set_sh_direct(&next,(uint32_t)(end-next),0x4c,
              runtime_vertex,state->runtime.vertex_count,sceAgcCbSetShRegisterRangeDirect))
            return VK_ERROR_UNKNOWN;
#endif
        if(ps5_agc_writer_set_sh_direct(&next,(uint32_t)(end-next),0x8c,
              runtime_vertex,state->runtime.vertex_count,sceAgcCbSetShRegisterRangeDirect) ||
           (state->runtime.fragment_count && ps5_agc_writer_set_sh_direct(&next,
              (uint32_t)(end-next),0xc,runtime_pixel,state->runtime.fragment_count,
              sceAgcCbSetShRegisterRangeDirect))) return VK_ERROR_UNKNOWN;
    } else if (ps5_agc_writer_set_sh_direct(&next, (uint32_t)(end-next), 0x8c,
            vertex_input ? vertex : procedural, vertex_input ? 4 : 3,
            sceAgcCbSetShRegisterRangeDirect) ||
        ps5_agc_writer_set_sh_direct(&next, (uint32_t)(end-next), 0xc, fragment, texture_low?2:1,
            sceAgcCbSetShRegisterRangeDirect)) return VK_ERROR_UNKNOWN;
    if (end-next < 5) return VK_ERROR_OUT_OF_HOST_MEMORY;
    /* Public Mesa sid.h/RADV: TYPE3 NUM_INSTANCES (opcode 0x2f), one payload.
     * Explicit on every draw, so prior application state cannot leak in. */
    *next++ = 0xc0002f00u; *next++ = op->instance_count;
    if(indices) {
        VkResult rc=ps5vk_native_emit_index(&next,(uint32_t)(end-next),indices,
            op->index_count,state->modifier,emit_index);
        if(rc!=VK_SUCCESS)return rc;
    } else if (ps5_agc_writer_draw_auto(&next, (uint32_t)(end-next), op->vertex_count,
            state->modifier, sceAgcDcbDrawIndexAuto)) return VK_ERROR_UNKNOWN;
    *cursor = next; return VK_SUCCESS;
}

VkResult ps5vk_native_emit_draw(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state,const void *mapping,size_t mapping_bytes,
    const struct ps5vk_operation *op,uint32_t global_table_low)
{ return emit_draw(cursor,capacity,state,mapping,mapping_bytes,op,global_table_low,0,0,NULL,NULL,NULL,NULL,NULL); }

VkResult ps5vk_native_emit_vertex_draw(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state,const void *mapping,size_t mapping_bytes,
    const struct ps5vk_operation *op,uint32_t global_table_low,uint32_t vertex_table_low)
{
    /* Only the audited four-user-SGPR compiler ABI. This low word is not a
     * substitute for the caller's full-address aperture/lifetime validation. */
    if(vertex_table_low%16)return VK_ERROR_UNKNOWN;
    return emit_draw(cursor,capacity,state,mapping,mapping_bytes,op,global_table_low,vertex_table_low,1,NULL,NULL,NULL,NULL,NULL);
}
VkResult ps5vk_native_emit_indexed_draw(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state,const void *mapping,size_t bytes,
    const struct ps5vk_operation *op,uint32_t global,uint32_t table,
    const struct ps5vk_index_fetch *indices,ps5vk_emit_index_fn emit)
{
    if(!indices || !emit || table%16)return VK_ERROR_UNKNOWN;
    return emit_draw(cursor,capacity,state,mapping,bytes,op,global,table,1,indices,emit,NULL,NULL,NULL);
}
VkResult ps5vk_native_emit_textured_draw(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state,const void *mapping,size_t bytes,
    const struct ps5vk_operation *op,uint32_t global,uint32_t vertex,uint32_t texture,
    const struct ps5vk_index_fetch *indices,ps5vk_emit_index_fn emit)
{
    if(vertex%16 || texture%16 || (indices && !emit))return VK_ERROR_UNKNOWN;
    return emit_draw(cursor,capacity,state,mapping,bytes,op,global,vertex,1,indices,emit,&texture,NULL,NULL);
}

VkResult ps5vk_native_emit_runtime_draw(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state,const void *mapping,size_t bytes,
    const struct ps5vk_operation *op,uint32_t vertex,
    const uint32_t tables[PS5VK_RUNTIME_DESCRIPTOR_SETS],
    const struct ps5vk_view_emit *view,
    const struct ps5vk_index_fetch *indices,ps5vk_emit_index_fn emit)
{
    if(!state || !state->runtime.enabled || !tables || vertex%16 || (indices && !emit))
        return VK_ERROR_UNKNOWN;
    return emit_draw(cursor,capacity,state,mapping,bytes,op,0,vertex,
        state->runtime.vertex_buffer_valid,indices,emit,NULL,tables,view);
}

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
#include "ps5_agc_writer.h"
#include "ps5_gpu_span.h"
#include <string.h>

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
    const struct ps5vk_index_fetch *indices,ps5vk_emit_index_fn emit_index,const uint32_t *texture_low)
{
    if (!cursor || !*cursor || !state || !op || op->type != (indices?PS5VK_DRAW_INDEXED:PS5VK_DRAW) ||
        !state->cx_count || state->cx_count > PS5VK_DRAW_CX_CAPACITY || !state->modifier ||
        !ps5_gpu_span_visible(mapping, mapping_bytes, state, sizeof(*state))) return VK_ERROR_UNKNOWN;
    /* Vulkan zero-count draws have no rasterization side effects. */
    if (!(indices?op->index_count:op->vertex_count) || !op->instance_count) return VK_SUCCESS;
    uint32_t runtime_vertex[16],runtime_pixel[16];
    uint32_t sh_count=state->sh_count?state->sh_count:12;
    if(sh_count>16)return VK_ERROR_UNKNOWN;
    if(state->runtime.enabled && (indices ||
        ps5vk_runtime_draw_values(&state->runtime,op->first_vertex,op->first_instance,
                                 vertex_input?vertex_table_low:0,state->push_constant_low,
                                 texture_low?*texture_low:0,
                                 runtime_vertex,runtime_pixel))) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (capacity < 13) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *next = *cursor, *end = next + capacity;
    if (ps5_agc_writer_set_indirect(&next, (uint32_t)(end-next), state->cx, state->cx_count,
            mapping, mapping_bytes, sceAgcDcbSetCxRegistersIndirect) ||
        ps5_agc_writer_set_indirect(&next, (uint32_t)(end-next), state->uc, 3,
            mapping, mapping_bytes, sceAgcDcbSetUcRegistersIndirect) ||
        ps5_agc_writer_set_indirect(&next, (uint32_t)(end-next), state->sh, sh_count,
            mapping, mapping_bytes, sceAgcDcbSetShRegistersIndirect)) return VK_ERROR_UNKNOWN;
#if defined(PS5VK_GRAPHICS_SCISSOR_PROBE) && PS5VK_GRAPHICS_SCISSOR_PROBE
    VkResult scissor_rc=ps5vk_native_emit_scissor_replay(&next,(uint32_t)(end-next),state);
    if(scissor_rc!=VK_SUCCESS)return scissor_rc;
#endif
    const uint32_t procedural[3] = {global_table_low, op->first_vertex, op->first_instance};
    const uint32_t vertex[4] = {global_table_low, vertex_table_low,
        indices?(uint32_t)op->vertex_offset:op->first_vertex, op->first_instance};
    const uint32_t fragment[2]={global_table_low,texture_low?*texture_low:0};
    if(state->runtime.enabled) {
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
{ return emit_draw(cursor,capacity,state,mapping,mapping_bytes,op,global_table_low,0,0,NULL,NULL,NULL); }

VkResult ps5vk_native_emit_vertex_draw(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state,const void *mapping,size_t mapping_bytes,
    const struct ps5vk_operation *op,uint32_t global_table_low,uint32_t vertex_table_low)
{
    /* Only the audited four-user-SGPR compiler ABI. This low word is not a
     * substitute for the caller's full-address aperture/lifetime validation. */
    if(vertex_table_low%16)return VK_ERROR_UNKNOWN;
    return emit_draw(cursor,capacity,state,mapping,mapping_bytes,op,global_table_low,vertex_table_low,1,NULL,NULL,NULL);
}
VkResult ps5vk_native_emit_indexed_draw(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state,const void *mapping,size_t bytes,
    const struct ps5vk_operation *op,uint32_t global,uint32_t table,
    const struct ps5vk_index_fetch *indices,ps5vk_emit_index_fn emit)
{
    if(!indices || !emit || table%16)return VK_ERROR_UNKNOWN;
    return emit_draw(cursor,capacity,state,mapping,bytes,op,global,table,1,indices,emit,NULL);
}
VkResult ps5vk_native_emit_textured_draw(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_draw_state *state,const void *mapping,size_t bytes,
    const struct ps5vk_operation *op,uint32_t global,uint32_t vertex,uint32_t texture,
    const struct ps5vk_index_fetch *indices,ps5vk_emit_index_fn emit)
{
    if(vertex%16 || texture%16 || (indices && !emit))return VK_ERROR_UNKNOWN;
    return emit_draw(cursor,capacity,state,mapping,bytes,op,global,vertex,1,indices,emit,&texture);
}

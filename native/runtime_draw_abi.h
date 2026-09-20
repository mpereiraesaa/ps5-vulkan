/*
 * Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The descriptor user-SGPR mapping follows the bounded PSBC/Gallium ABI in
 * the pinned ps5-opengl reference. Validation and draw ownership are local.
 */
#ifndef PS5VK_RUNTIME_DRAW_ABI_H
#define PS5VK_RUNTIME_DRAW_ABI_H
#include <stdint.h>
#include <string.h>
#define PS5VK_RUNTIME_DESCRIPTOR_SETS 4

/* Bounded runtime profile. UINT32_MAX denotes an unused argument. Counts and
 * slots originate from compiler metadata, not pipeline filenames. */
struct ps5vk_runtime_draw_abi {
    uint32_t enabled;
    uint32_t vertex_count, fragment_count;
    /* Compiler-declared user-SGPR dwords, UINT32_MAX when the stage does not
     * read that value. draw_id_slot is the DrawIndex slot (metadata v13); a
     * stage that reads the built-in always has a real slot here, so a caller
     * cannot leave the built-in unwritten by accident. view_index_slot is the
     * vertex multiview ViewIndex slot (metadata v14), present only when read.
     * A zero-initialised struct names slot 0 for every one of these fields and
     * is therefore rejected as a collision, never silently accepted. */
    uint32_t base_vertex_slot, start_instance_slot, draw_id_slot, view_index_slot;
    uint32_t fragment_view_index_valid, fragment_view_index_slot;
    uint32_t vertex_buffer_valid, vertex_buffer_slot;
    uint32_t vertex_buffer_usage_mask;
    uint32_t lds_slot, lds_value;
    /* The SGPR index this stage's first user-data dword lands on, as published
     * by the compiler (zero = not declared). The driver writes exactly
     * vertex_count/fragment_count dwords starting there, so every slot below is
     * window-relative and a slot at or beyond window_base+count would run past
     * the block. */
    uint32_t window_base;
    /* LS/HS has up to16 user dwords after its eight system SGPRs. */
    uint32_t hull;
    /* A merged vertex+geometry program gates and sizes its halves from two
     * SYSTEM SGPRs, which were measured below the driver's user-data window on
     * three compiled programs: a merged pair and a clip/cull vertex program both
     * carry gs_tg_info in SGPR 2 and merged_wave_info in SGPR 3 with the window
     * starting in SGPR 8, and the hardware-verified draw-parameter program
     * carries BaseVertex/BaseInstance/DrawIndex in the same window-relative
     * slots (0/1 at SGPR 9/8, 2 at SGPR 10, 3 at SGPR 11). The driver cannot
     * address the system block, so these two indices are recorded as a contract
     * check - they must fall below window_base - and are never written. */
    uint32_t esgs_described;
    uint32_t esgs_gs_tg_info_sgpr, esgs_merged_wave_info_sgpr;
    uint32_t vertex_push_slot, fragment_push_slot, push_constant_size;
    /* The tessellation ring descriptor table's address, for the DOMAIN half.
     *
     * This block used to assert the opposite - that the hardware hands a
     * merged NGG program its ring bases from the device ring state, so no
     * field here needed to carry the address. The compiler says otherwise
     * and the compiler is the one that decides: the evaluation half's
     * metadata reports ps5_ring_table_valid with a window-relative dword,
     * because on this platform the system-block ring_offsets at s0/s1 is not
     * writable and RADV's PS5 path therefore declares the table as a user
     * SGPR pair for BOTH tessellation stages, not just the hull. Measured at
     * the packet level: the pre-raster stage's user-data dwords were written
     * as all zero at SPI_SHADER_USER_DATA_GS_0, so a domain that dereferences
     * its table dereferences NULL.
     *
     * The slot is window-relative and consumes TWO dwords, low then high, so
     * both are reserved against collisions below. The address is a pipeline
     * property rather than a per-draw one, so it travels in this struct
     * beside the slot instead of through the value builder's parameters. */
    uint32_t ring_table_valid, ring_table_slot;
    uint32_t ring_table_low, ring_table_high;
    /* DIAGNOSTIC (legacy hardware-VS launch): 1 when the pre-raster program
     * that launches is a legacy hardware VS, whose user data lives at
     * SPI_SHADER_USER_DATA_VS_0 with no system-SGPR preamble. */
    uint32_t legacy_vs;
    uint32_t vertex_descriptor_valid[PS5VK_RUNTIME_DESCRIPTOR_SETS];
    uint32_t fragment_descriptor_valid[PS5VK_RUNTIME_DESCRIPTOR_SETS];
    uint32_t vertex_descriptor_slot[PS5VK_RUNTIME_DESCRIPTOR_SETS];
    uint32_t fragment_descriptor_slot[PS5VK_RUNTIME_DESCRIPTOR_SETS];
    /* Bindings the compiled stages really dereference, per set: bit B is set
     * when binding B of that table is read by that stage. A zero entry for a
     * valid set means the compiler could not name the bindings, so the whole
     * declaration of that set stays required (fail closed). */
    uint64_t vertex_used_bindings[PS5VK_RUNTIME_DESCRIPTOR_SETS];
    uint64_t fragment_used_bindings[PS5VK_RUNTIME_DESCRIPTOR_SETS];
};

/* All validation precedes publication of either register bank. A set has one
 * table address, shared by all stages that statically use that set. */
static inline int ps5vk_runtime_draw_values_sets(const struct ps5vk_runtime_draw_abi *a,
    uint32_t base_vertex, uint32_t instance, uint32_t draw_index, uint32_t view_index,
    uint32_t vertex_buffer_low,
    uint32_t push_constant_low, const uint32_t descriptor_low[PS5VK_RUNTIME_DESCRIPTOR_SETS],
    uint32_t vertex_out[16], uint32_t pixel_out[16])
{
    if (!a || !descriptor_low || !vertex_out || !pixel_out || a->enabled!=1 || !a->vertex_count || a->vertex_count>16 ||
        a->fragment_count>16 ||
        (a->lds_slot!=UINT32_MAX && a->lds_slot>=a->vertex_count) ||
        (a->lds_slot==UINT32_MAX && a->lds_value) || a->lds_value>UINT16_MAX)
        return -1;
    /* The window base places the whole block: a block that cannot fit below
     * SGPR 16 is unusable, and the two system registers a merged pair gates on
     * must lie outside it. A pair that claims they are window-relative is
     * refused rather than written into user data the shader never reads. */
    if(a->hull>1 || (a->hull && a->window_base!=8))return -1;
    if(a->window_base && (a->window_base>16 ||
        a->window_base+a->vertex_count>(a->hull?24u:16u))) return -1;
    if(a->esgs_described>1) return -1;
    if(a->esgs_described) {
        if(!a->window_base || a->esgs_gs_tg_info_sgpr>=a->window_base ||
           a->esgs_merged_wave_info_sgpr>=a->window_base ||
           a->esgs_gs_tg_info_sgpr==a->esgs_merged_wave_info_sgpr) return -1;
    }
    if(a->vertex_buffer_valid>1)return -1;
    if(a->vertex_buffer_valid ? (!a->vertex_buffer_usage_mask || a->vertex_buffer_usage_mask>0xffffu) :
       a->vertex_buffer_usage_mask!=0)return -1;
    if(a->ring_table_valid>1)return -1;
    if(!a->ring_table_valid && (a->ring_table_slot || a->ring_table_low ||
        a->ring_table_high))return -1;
    /* The table is a 64-bit pointer: its high dword sits in the next slot, so
     * the pair has to fit inside the window and both halves have to be
     * checked for collisions, not just the first. */
    if(a->ring_table_valid && a->ring_table_slot+1u>=a->vertex_count)return -1;
    uint32_t slots[9]={a->base_vertex_slot,a->start_instance_slot,a->draw_id_slot,
        a->view_index_slot,
        a->vertex_buffer_valid?a->vertex_buffer_slot:UINT32_MAX,
        a->lds_slot,a->vertex_push_slot,
        a->ring_table_valid?a->ring_table_slot:UINT32_MAX,
        a->ring_table_valid?a->ring_table_slot+1u:UINT32_MAX};
    for(unsigned i=0;i<9;++i) {
        if(slots[i]==UINT32_MAX)continue;
        if(slots[i]>=a->vertex_count)return -1;
        for(unsigned j=0;j<i;++j)if(slots[i]==slots[j])return -1;
    }
    uint32_t vertex[16]={0},pixel[16]={0};
    uint32_t vertex_used=0,pixel_used=0;
    if(a->fragment_view_index_valid>1 ||
       (a->fragment_view_index_valid ? a->fragment_view_index_slot>=a->fragment_count :
        a->fragment_view_index_slot!=0))return -1;
    if(a->fragment_view_index_valid) {
        pixel[a->fragment_view_index_slot]=view_index;
        pixel_used|=1u<<a->fragment_view_index_slot;
    }
    for(unsigned i=0;i<9;++i)if(slots[i]!=UINT32_MAX)vertex_used|=1u<<slots[i];
    if(a->base_vertex_slot!=UINT32_MAX)vertex[a->base_vertex_slot]=base_vertex;
    if(a->start_instance_slot!=UINT32_MAX)vertex[a->start_instance_slot]=instance;
    if(a->draw_id_slot!=UINT32_MAX)vertex[a->draw_id_slot]=draw_index;
    if(a->view_index_slot!=UINT32_MAX)vertex[a->view_index_slot]=view_index;
    if(a->vertex_buffer_valid) {
        if(!vertex_buffer_low || (vertex_buffer_low&15u))return -1;
        vertex[a->vertex_buffer_slot]=vertex_buffer_low;
    } else if(vertex_buffer_low)return -1;
    if(a->lds_slot!=UINT32_MAX)vertex[a->lds_slot]=a->lds_value;
    if(a->ring_table_valid) {
        vertex[a->ring_table_slot]=a->ring_table_low;
        vertex[a->ring_table_slot+1u]=a->ring_table_high;
    }
    if(a->push_constant_size) {
        if(!push_constant_low || (push_constant_low&3u) || a->push_constant_size>256 ||
           (a->vertex_push_slot==UINT32_MAX && a->fragment_push_slot==UINT32_MAX) ||
           (a->fragment_push_slot!=UINT32_MAX && a->fragment_push_slot>=a->fragment_count))return -1;
        if(a->vertex_push_slot!=UINT32_MAX)vertex[a->vertex_push_slot]=push_constant_low;
        if(a->fragment_push_slot!=UINT32_MAX) {
            if(pixel_used&(1u<<a->fragment_push_slot))return -1;
            pixel[a->fragment_push_slot]=push_constant_low;
            pixel_used|=1u<<a->fragment_push_slot;
        }
    } else if(a->vertex_push_slot!=UINT32_MAX || a->fragment_push_slot!=UINT32_MAX || push_constant_low)return -1;
    for(unsigned s=0;s<PS5VK_RUNTIME_DESCRIPTOR_SETS;++s) {
        uint32_t vv=a->vertex_descriptor_valid[s],fv=a->fragment_descriptor_valid[s];
        if(vv>1 || fv>1)return -1;
        if(vv || fv) {
            if(!descriptor_low[s] || (descriptor_low[s]&15u))return -1;
        } else if(descriptor_low[s])return -1;
        if(vv) {
            uint32_t slot=a->vertex_descriptor_slot[s];
            if(slot>=a->vertex_count || (vertex_used&(1u<<slot)))return -1;
            vertex_used|=1u<<slot;vertex[slot]=descriptor_low[s];
        }
        if(fv) {
            uint32_t slot=a->fragment_descriptor_slot[s];
            if(slot>=a->fragment_count || (pixel_used&(1u<<slot)))return -1;
            pixel_used|=1u<<slot;pixel[slot]=descriptor_low[s];
        }
    }
    memcpy(vertex_out,vertex,sizeof(vertex));memcpy(pixel_out,pixel,sizeof(pixel));
    return 0;
}

/* Compatibility bridge for existing one-table command emitters. A compiled
 * multi-set program cannot pass here with missing addresses. The view index is
 * passed explicitly as zero for non-multiview draws; multiview uses the
 * sets entry point to deliver each replay's real index to both stages. */
static inline int ps5vk_runtime_draw_values(const struct ps5vk_runtime_draw_abi *a,
    uint32_t base_vertex,uint32_t instance,uint32_t draw_index,uint32_t vertex_buffer_low,
    uint32_t push_constant_low,uint32_t descriptor_set0_low,
    uint32_t vertex[16],uint32_t pixel[16])
{
    const uint32_t tables[PS5VK_RUNTIME_DESCRIPTOR_SETS]={descriptor_set0_low,0,0,0};
    return ps5vk_runtime_draw_values_sets(a,base_vertex,instance,draw_index,0u,
        vertex_buffer_low,push_constant_low,tables,vertex,pixel);
}
#endif

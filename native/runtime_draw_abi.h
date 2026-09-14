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

/* Bounded runtime profile. UINT32_MAX denotes an unused argument. Counts and
 * slots originate from compiler metadata, not pipeline filenames. */
struct ps5vk_runtime_draw_abi {
    uint32_t enabled;
    uint32_t vertex_count, fragment_count;
    uint32_t base_vertex_slot, start_instance_slot;
    uint32_t vertex_buffer_valid, vertex_buffer_slot;
    uint32_t vertex_buffer_usage_mask;
    uint32_t lds_slot, lds_value;
    uint32_t vertex_push_slot, fragment_push_slot, push_constant_size;
    uint32_t fragment_descriptor_set0_valid, fragment_descriptor_set0_slot;
};

static inline int ps5vk_runtime_draw_values(const struct ps5vk_runtime_draw_abi *a,
    uint32_t base_vertex, uint32_t instance, uint32_t vertex_buffer_low,
    uint32_t push_constant_low, uint32_t descriptor_set0_low,
    uint32_t vertex[16], uint32_t pixel[16])
{
    if (!a || a->enabled!=1 || !a->vertex_count || a->vertex_count>16 ||
        a->fragment_count>16 || a->lds_slot>=a->vertex_count || a->lds_value>UINT16_MAX)
        return -1;
    if(a->vertex_buffer_valid>1 || a->fragment_descriptor_set0_valid>1)return -1;
    if(a->vertex_buffer_valid ? (!a->vertex_buffer_usage_mask || a->vertex_buffer_usage_mask>0xffffu) :
       a->vertex_buffer_usage_mask!=0)return -1;
    uint32_t slots[5]={a->base_vertex_slot,a->start_instance_slot,
        a->vertex_buffer_valid?a->vertex_buffer_slot:UINT32_MAX,
        a->lds_slot,a->vertex_push_slot};
    for(unsigned i=0;i<5;++i) {
        if(slots[i]==UINT32_MAX)continue;
        if(slots[i]>=a->vertex_count)return -1;
        for(unsigned j=0;j<i;++j)if(slots[i]==slots[j])return -1;
    }
    for(unsigned i=0;i<16;++i)vertex[i]=pixel[i]=0;
    if(a->base_vertex_slot!=UINT32_MAX)vertex[a->base_vertex_slot]=base_vertex;
    if(a->start_instance_slot!=UINT32_MAX)vertex[a->start_instance_slot]=instance;
    if(a->vertex_buffer_valid) {
        if(!vertex_buffer_low || (vertex_buffer_low&15u))return -1;
        vertex[a->vertex_buffer_slot]=vertex_buffer_low;
    } else if(vertex_buffer_low)return -1;
    vertex[a->lds_slot]=a->lds_value;
    if(a->push_constant_size) {
        if(!push_constant_low || (push_constant_low&3u) || a->push_constant_size>256 ||
           (a->vertex_push_slot==UINT32_MAX && a->fragment_push_slot==UINT32_MAX) ||
           (a->fragment_push_slot!=UINT32_MAX && a->fragment_push_slot>=a->fragment_count))return -1;
        if(a->vertex_push_slot!=UINT32_MAX)vertex[a->vertex_push_slot]=push_constant_low;
        if(a->fragment_push_slot!=UINT32_MAX)pixel[a->fragment_push_slot]=push_constant_low;
    } else if(a->vertex_push_slot!=UINT32_MAX || a->fragment_push_slot!=UINT32_MAX || push_constant_low)return -1;
    if(a->fragment_descriptor_set0_valid) {
        if(!descriptor_set0_low || (descriptor_set0_low&15u) ||
           a->fragment_descriptor_set0_slot>=a->fragment_count ||
           a->fragment_descriptor_set0_slot==a->fragment_push_slot)return -1;
        pixel[a->fragment_descriptor_set0_slot]=descriptor_set0_low;
    } else if(descriptor_set0_low)return -1;
    return 0;
}
#endif

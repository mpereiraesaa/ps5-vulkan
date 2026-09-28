/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Manuel Pereira
 */
#ifndef PS5VK_INLINE_UNIFORM_DESCRIPTOR_H
#define PS5VK_INLINE_UNIFORM_DESCRIPTOR_H
#include "descriptor_table_layout.h"

/* Encode into scratch while pointing at the final GPU-visible table. The
 * caller retains that table until completion. Failure leaves scratch intact. */
static inline VkResult ps5vk_inline_uniform_descriptor(VkDevice device, VkDescriptorSet set,
    uint32_t binding, uint32_t table_dword, uint32_t *table, uint32_t *scratch,
    size_t capacity_dwords)
{
    if (!device || !device->inline_uniform_block_enabled || !set || !set->pool ||
        set->pool->device != device || !set->defined || !table || !scratch || binding >= PS5VK_MAX_BINDINGS ||
        table_dword % 4 || capacity_dwords > PS5VK_MAX_TABLE_DWORDS) return VK_ERROR_UNKNOWN;
    const uint32_t span = ps5vk_descriptor_span_dwords(&set->signature, binding,
        VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);
    const uint32_t bytes = set->signature.inline_bytes[binding];
    const uint32_t offset = set->inline_uniform.offset[binding];
    const uint32_t slot = set->signature.binding[binding].first;
    struct ps5vk_descriptor_table_layout layout;
    if (!span || span > capacity_dwords || table_dword > capacity_dwords - span ||
        slot >= set->capacity || !set->defined[slot] ||
        set->inline_uniform.bytes[binding] != bytes ||
        set->inline_uniform.total_bytes > sizeof(set->inline_data) ||
        offset > set->inline_uniform.total_bytes || bytes > set->inline_uniform.total_bytes - offset ||
        ps5vk_descriptor_table_layout_build(1, &set->signature, &layout) != VK_SUCCESS ||
        layout.binding[0][binding].byte_offset / 4 != table_dword) return VK_ERROR_UNKNOWN;
    uint32_t expected_offset = 0;
    for (uint32_t b = 0; b < binding; ++b) expected_offset += set->signature.inline_bytes[b];
    if (offset != expected_offset) return VK_ERROR_UNKNOWN;
    const uint64_t base = (uintptr_t)table, end = UINT64_C(1) << 48;
    const uint64_t relative = ((uint64_t)table_dword + 4u) * 4u;
    if (base >= end || relative > end - base || bytes > end - base - relative)
        return VK_ERROR_UNKNOWN;
    const uint64_t address = base + relative;
    uint32_t *out = scratch + table_dword;
    out[0] = (uint32_t)address; out[1] = (uint32_t)(address >> 32);
    out[2] = bytes; out[3] = 0x31016fac;
    memset(out + 4, 0, (span - 4u) * 4u);
    memcpy(out + 4, set->inline_data + offset, bytes);
    return VK_SUCCESS;
}

#endif

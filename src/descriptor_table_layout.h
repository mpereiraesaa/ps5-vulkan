/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Manuel Pereira
 */
#ifndef PS5VK_DESCRIPTOR_TABLE_LAYOUT_H
#define PS5VK_DESCRIPTOR_TABLE_LAYOUT_H
#include "vk_descriptor.h"
#include <string.h>

/* One canonical byte layout for compiler declarations and native table
 * preparation. The layout itself does not enable a shader/resource profile.
 * It describes the descriptor types our encoders actually implement: four
 * DWORD buffer SRDs, twelve DWORD combined T#/S# records and an eight DWORD
 * resource-only image record for input attachments, which carries no sampler
 * payload at all. Empty bindings retain their prefix offset but never consume
 * table storage.
 *
 * Stage filtering must NOT compact offsets: one set is shared by stages,
 * even when their statically used binding subsets differ. */
struct ps5vk_descriptor_table_binding {
    uint32_t byte_offset, byte_stride;
};
struct ps5vk_descriptor_table_layout {
    struct ps5vk_descriptor_table_binding binding[PS5VK_MAX_SETS][PS5VK_MAX_BINDINGS];
    uint32_t set_bytes[PS5VK_MAX_SETS];
    uint32_t binding_count, descriptor_count;
};

static inline uint32_t ps5vk_descriptor_record_bytes(VkDescriptorType type)
{
    switch (type) {
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return 48;
    /* Resource-only image record: the same GFX10 image fields, no S# words. */
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return 32;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return 16;
    default: return 0;
    }
}

/* Failure leaves the output untouched. Every input signature must have the
 * same canonical prefix indices as vkCreateDescriptorSetLayout. */
static inline VkResult ps5vk_descriptor_table_layout_build(uint32_t set_count,
    const struct ps5vk_set_signature *sets, struct ps5vk_descriptor_table_layout *out)
{
    if (!out || (set_count && !sets)) return VK_ERROR_UNKNOWN;
    if (set_count > PS5VK_MAX_SETS) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_descriptor_table_layout result = {0};
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    for (uint32_t s = 0; s < set_count; ++s) {
        uint32_t prefix = 0, offset = 0;
        if (sets[s].count > PS5VK_MAX_DESCRIPTORS) return VK_ERROR_FEATURE_NOT_PRESENT;
        for (uint32_t b = 0; b < PS5VK_MAX_BINDINGS; ++b) {
            const struct ps5vk_binding *binding = &sets[s].binding[b];
            if (binding->first != prefix ||
                binding->count > PS5VK_MAX_DESCRIPTORS - prefix) return VK_ERROR_UNKNOWN;
            result.binding[s][b].byte_offset = offset;
            if (!binding->count) {
                if (binding->stages || sets[s].type[b]) return VK_ERROR_UNKNOWN;
                continue;
            }
            /* Visibility does not instantiate a shader stage. ALL is a
             * reserved convenience mask, not merely the OR of core bits. */
            if (!binding->stages || (binding->stages != VK_SHADER_STAGE_ALL &&
                    (binding->stages & ~stages))) return VK_ERROR_UNKNOWN;
            uint32_t stride = ps5vk_descriptor_record_bytes(sets[s].type[b]);
            if (!stride) return VK_ERROR_FEATURE_NOT_PRESENT;
            if (binding->count > (UINT32_MAX - offset) / stride) return VK_ERROR_UNKNOWN;
            result.binding[s][b].byte_stride = stride;
            offset += stride * binding->count;
            prefix += binding->count;
            ++result.binding_count;
        }
        if (prefix != sets[s].count) return VK_ERROR_UNKNOWN;
        result.set_bytes[s] = offset;
        result.descriptor_count += prefix;
    }
    *out = result;
    return VK_SUCCESS;
}
#endif

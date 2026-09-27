/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Manuel Pereira
 */
#ifndef PS5VK_SPIRV_DESCRIPTOR_TYPES_H
#define PS5VK_SPIRV_DESCRIPTOR_TYPES_H
#include "vk_descriptor.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Every opaque resource a module declares at a (set, binding) the layout
 * names must be the descriptor type that binding holds. The compiler reads
 * each record at its binding's offset with the width its SPIR-V type implies,
 * so a combined sampler2D over a SAMPLED_IMAGE binding, or a separate sampler
 * over a combined binding, would read the wrong words instead of failing.
 * Vulkan makes such a pipeline invalid; it is refused here.
 *
 * Classification (arrays unwrapped):
 *   OpTypeSampler                         -> SAMPLER
 *   OpTypeSampledImage of a Buffer image  -> UNIFORM_TEXEL_BUFFER
 *   OpTypeSampledImage of any other image -> COMBINED_IMAGE_SAMPLER
 *   OpTypeImage Buffer, Sampled 1 / 2     -> UNIFORM / STORAGE_TEXEL_BUFFER
 *   OpTypeImage SubpassData               -> INPUT_ATTACHMENT
 *   OpTypeImage other, Sampled 1 / 2      -> SAMPLED_IMAGE / STORAGE_IMAGE
 * Block-typed Uniform/StorageBuffer variables are buffers and are left to the
 * existing buffer checks. Inline blocks must be direct Uniform Block structs,
 * never storage buffers or arrays of descriptor blocks. Returns 0 on malformed
 * input or any mismatch. */
static inline int ps5vk_spirv_opaque_descriptor_type(const uint32_t *words, uint32_t bound,
    const uint32_t *type_at, uint32_t id, VkDescriptorType *out)
{
    for (unsigned depth = 0; depth < 4; ++depth) {
        if (id >= bound || !type_at[id]) return 0;
        const uint32_t *w = words + type_at[id];
        const uint32_t n = w[0] >> 16, op = w[0] & 0xffffu;
        if (op == 28u || op == 29u) { id = w[2]; continue; } /* (runtime) array */
        if (op == 26u) { *out = VK_DESCRIPTOR_TYPE_SAMPLER; return 1; }
        if (op == 27u) {
            if (n < 3 || w[2] >= bound || !type_at[w[2]]) return -1;
            const uint32_t *image = words + type_at[w[2]];
            if ((image[0] & 0xffffu) != 25u || (image[0] >> 16) < 9) return -1;
            *out = image[3] == 5u ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            return 1;
        }
        if (op == 25u) {
            if (n < 9) return -1;
            const uint32_t dim = w[3], sampled = w[7];
            if (dim == 6u) *out = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            else if (dim == 5u) *out = sampled == 2u ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
                VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else *out = sampled == 2u ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            return 1;
        }
        return 0; /* not an opaque resource */
    }
    return -1;
}

struct ps5vk_spirv_resource_decorations {
    uint32_t set, binding;
    unsigned seen, block, buffer_block;
};

/* Resolve resource decorations applied directly or through OpGroupDecorate.
 * Instruction bounds and group ids have already been checked by the scanner.
 * GroupMemberDecorate addresses members, never descriptor variables/types. */
static inline int ps5vk_spirv_resource_decorations(const uint32_t *words, size_t count,
    const uint32_t *type_at, uint32_t bound, uint32_t id,
    struct ps5vk_spirv_resource_decorations *out)
{
    struct ps5vk_spirv_resource_decorations result = {UINT32_MAX, UINT32_MAX, 0, 0, 0};
    for (size_t at = 5; at < count; at += words[at] >> 16) {
        const uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (op != 71u || n < 3) continue;
        const uint32_t target = words[at + 1], decoration = words[at + 2];
        if (decoration != 2u && decoration != 3u && decoration != 33u && decoration != 34u)
            continue;
        int applies = target == id;
        if (!applies && target < bound && type_at[target] &&
            (words[type_at[target]] & 0xffffu) == 73u) {
            for (size_t g = 5; g < count && !applies; g += words[g] >> 16)
                if ((words[g] & 0xffffu) == 74u && words[g + 1] == target)
                    for (uint32_t t = 2; t < words[g] >> 16; ++t)
                        if (words[g + t] == id) applies = 1;
        }
        if (!applies) continue;
        if (decoration == 2u || decoration == 3u) {
            if (n != 3) return 0;
            if (decoration == 2u) result.block = 1;
            else result.buffer_block = 1;
        } else {
            if (n != 4) return 0;
            const unsigned bit = decoration == 34u ? 1u : 2u;
            uint32_t *value = decoration == 34u ? &result.set : &result.binding;
            if ((result.seen & bit) && *value != words[at + 3]) return 0;
            *value = words[at + 3]; result.seen |= bit;
        }
    }
    *out = result;
    return 1;
}

static inline int ps5vk_spirv_descriptor_types_scan(const uint32_t *words, size_t count,
    uint32_t set_count, const struct ps5vk_set_signature *sets, uint32_t *type_at,
    uint32_t bound)
{
    /* First pass: type and pointer definitions by result id. */
    for (size_t at = 5; at < count;) {
        const uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (!n || n > count - at) return 0;
        if ((op >= 25u && op <= 30u) || op == 32u || op == 73u) {
            if (n < 2 || words[at + 1] >= bound || (op == 73u && n != 2)) return 0;
            type_at[words[at + 1]] = (uint32_t)at;
        }
        at += n;
    }
    /* Group targets cannot themselves be decoration groups. This also rules
     * out cycles before resolving applications, independent of declaration order. */
    for (size_t at = 5; at < count; at += words[at] >> 16) {
        const uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (op != 74u) continue;
        if (n < 3 || words[at + 1] >= bound || !type_at[words[at + 1]] ||
            (words[type_at[words[at + 1]]] & 0xffffu) != 73u) return 0;
        for (uint32_t t = 2; t < n; ++t) {
            const uint32_t id = words[at + t];
            if (!id || id >= bound || (type_at[id] &&
                (words[type_at[id]] & 0xffffu) == 73u)) return 0;
        }
    }
    /* Every decorated variable is checked against the layout. */
    for (size_t at = 5; at < count;) {
        const uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (op == 59u && n >= 4) {
            const uint32_t pointer = words[at + 1], id = words[at + 2];
            struct ps5vk_spirv_resource_decorations resource;
            if (!ps5vk_spirv_resource_decorations(words, count, type_at, bound, id, &resource)) return 0;
            const uint32_t set = resource.set, binding = resource.binding;
            if (set < set_count && binding < PS5VK_MAX_BINDINGS &&
                sets[set].binding[binding].count) {
                if (pointer >= bound || !type_at[pointer]) return 0;
                const uint32_t *p = words + type_at[pointer];
                if ((p[0] & 0xffffu) != 32u || (p[0] >> 16) < 4) return 0;
                if (sets[set].type[binding] == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
                    if (words[at + 3] != 2u || p[2] != 2u || p[3] >= bound ||
                        !type_at[p[3]] ||
                        (words[type_at[p[3]]] & 0xffffu) != 30u) return 0;
                    struct ps5vk_spirv_resource_decorations block;
                    if (!ps5vk_spirv_resource_decorations(words, count, type_at, bound, p[3], &block) ||
                        !block.block || block.buffer_block) return 0;
                    at += n;
                    continue;
                }
                VkDescriptorType type;
                const int opaque = ps5vk_spirv_opaque_descriptor_type(words, bound,
                    type_at, p[3], &type);
                if (opaque < 0) return 0;
                if (opaque && type != sets[set].type[binding]) return 0;
            }
        }
        at += n;
    }
    return 1;
}

static inline int ps5vk_spirv_descriptor_types_match(const uint32_t *words, size_t count,
    uint32_t set_count, const struct ps5vk_set_signature *sets)
{
    if (!words || count < 5 || words[0] != 0x07230203u || (set_count && !sets) ||
        !words[3] || words[3] > (1u << 22)) return 0;
    uint32_t *type_at = calloc(words[3], sizeof(*type_at));
    if (!type_at) return 0;
    const int ok = ps5vk_spirv_descriptor_types_scan(words, count, set_count, sets,
        type_at, words[3]);
    free(type_at);
    return ok;
}
#endif

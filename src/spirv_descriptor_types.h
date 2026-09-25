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
 * existing buffer checks. Returns 0 on malformed input or any mismatch. */
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

static inline int ps5vk_spirv_descriptor_types_scan(const uint32_t *words, size_t count,
    uint32_t set_count, const struct ps5vk_set_signature *sets, uint32_t *type_at,
    uint32_t bound)
{
    /* First pass: type and pointer definitions by result id. */
    for (size_t at = 5; at < count;) {
        const uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (!n || n > count - at) return 0;
        if ((op >= 25u && op <= 29u) || op == 32u) {
            if (n < 2 || words[at + 1] >= bound) return 0;
            type_at[words[at + 1]] = (uint32_t)at;
        }
        at += n;
    }
    /* Second pass: every decorated variable against the layout. Decorations
     * precede the variables, so the (set, binding) of an id is gathered from
     * the decoration section on demand. */
    for (size_t at = 5; at < count;) {
        const uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (op == 59u && n >= 4) {
            const uint32_t pointer = words[at + 1], id = words[at + 2];
            uint32_t set = UINT32_MAX, binding = UINT32_MAX;
            for (size_t d = 5; d < count;) {
                const uint32_t dn = words[d] >> 16, dop = words[d] & 0xffffu;
                if (dop == 71u && dn == 4 && words[d + 1] == id) {
                    if (words[d + 2] == 34u) set = words[d + 3];
                    if (words[d + 2] == 33u) binding = words[d + 3];
                }
                d += dn;
            }
            if (set < set_count && binding < PS5VK_MAX_BINDINGS &&
                sets[set].binding[binding].count && pointer < bound &&
                type_at[pointer]) {
                const uint32_t *p = words + type_at[pointer];
                if ((p[0] & 0xffffu) != 32u || (p[0] >> 16) < 4) return 0;
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

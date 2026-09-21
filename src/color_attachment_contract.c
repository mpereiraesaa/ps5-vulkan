/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "color_attachment_contract.h"

/* Target 0 is the block ps5-agc-gears ps5_color_target.c builds; target 1 is
 * the same registers one MRT further (see the header). Both are recomputed
 * from the pinned register table by tests/test_color_attachment_offsets.py. */
const uint32_t ps5vk_color_target_offsets[2][PS5VK_COLOR_TARGET_REGISTERS] = {
    {0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f, 0x321, 0x323,
     0x324, 0x325, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8},
    {0x327, 0x32a, 0x32b, 0x32c, 0x32d, 0x32e, 0x330, 0x332,
     0x333, 0x334, 0x391, 0x399, 0x3a1, 0x3a9, 0x3b1, 0x3b9},
};

int ps5vk_color_attachment_count_supported(uint32_t count)
{
    return count == (uint32_t)PS5VK_MAX_COLOR_ATTACHMENTS;
}

static int blend_factor_uses_src1(VkBlendFactor factor)
{
    return factor == VK_BLEND_FACTOR_SRC1_COLOR ||
        factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
        factor == VK_BLEND_FACTOR_SRC1_ALPHA ||
        factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}

int ps5vk_color_attachment_uses_src1(const VkPipelineColorBlendAttachmentState *state)
{
    return state && state->blendEnable &&
        (blend_factor_uses_src1(state->srcColorBlendFactor) ||
         blend_factor_uses_src1(state->dstColorBlendFactor) ||
         blend_factor_uses_src1(state->srcAlphaBlendFactor) ||
         blend_factor_uses_src1(state->dstAlphaBlendFactor));
}

int ps5vk_color_blend_state_shape_supported(const VkPipelineColorBlendStateCreateInfo *state)
{
    if (!state || state->pNext || state->flags) return 0;
    /* Logic operations are a D3D10-era fixed function this profile never
     * programs; the d3d11 baseline does not require them. */
    if (state->logicOpEnable) return 0;
    if (!ps5vk_color_attachment_count_supported(state->attachmentCount)) return 0;
    /* Vulkan reads pAttachments[0] once the count is one, so a missing array is
     * malformed rather than empty. */
    return state->pAttachments != NULL;
}

/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "color_attachment_contract.h"

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

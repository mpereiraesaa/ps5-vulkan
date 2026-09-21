/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "color_attachment_contract.h"
#include <assert.h>
#include <stddef.h>

int main(void)
{
    /* The profile serves exactly one colour attachment: that bound is what
     * keeps independentBlend a blocker, and a slice that can render a second
     * target is the one allowed to raise it. */
    /* The ABI describes up to PS5VK_MAX_COLOR_ATTACHMENTS; the profile serves
     * fewer until the native second target lands, which the adapter enforces. */
    assert(ps5vk_color_attachment_count_supported(PS5VK_MAX_COLOR_ATTACHMENTS));
    assert(ps5vk_color_attachment_count_supported(1));
    assert(!ps5vk_color_attachment_count_supported(0));
    assert(!ps5vk_color_attachment_count_supported(PS5VK_MAX_COLOR_ATTACHMENTS + 1));
    assert(!ps5vk_color_attachment_count_supported(8));

    VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo state = {
        .attachmentCount = 1, .pAttachments = &attachment};
    assert(ps5vk_color_blend_state_shape_supported(&state));

    /* Logic operations are not programmed and the d3d11 baseline never needs
     * them; a count the native path cannot render is refused; a missing
     * attachment array is malformed, not empty. */
    state.logicOpEnable = VK_TRUE;
    assert(!ps5vk_color_blend_state_shape_supported(&state));
    state.logicOpEnable = VK_FALSE;
    /* The ABI can describe the widest count it carries; what the profile
     * serves is narrower and enforced by the adapter. */
    state.attachmentCount = PS5VK_MAX_COLOR_ATTACHMENTS;
    assert(ps5vk_color_blend_state_shape_supported(&state));
    state.attachmentCount = PS5VK_MAX_COLOR_ATTACHMENTS + 1;
    assert(!ps5vk_color_blend_state_shape_supported(&state));
    state.attachmentCount = 1;
    state.pAttachments = NULL;
    assert(!ps5vk_color_blend_state_shape_supported(&state));
    state.pAttachments = &attachment;
    assert(!ps5vk_color_blend_state_shape_supported(NULL));

    /* SPI_SHADER_COL_FORMAT is one nibble per target: FP16_ABGR when the
     * target blends, 32_ABGR when it does not. The pinned compiler reproduces
     * both codes (measured: option 0 -> 0x9, option 4 -> 0x4 for a
     * single-output module), and a second target's code rides in the second
     * nibble. */
    assert(ps5vk_color_export_format_code(1) == 4u);
    assert(ps5vk_color_export_format_code(0) == 9u);
    {
        const unsigned char plain[PS5VK_MAX_COLOR_ATTACHMENTS] = {0};
        const unsigned char blended[PS5VK_MAX_COLOR_ATTACHMENTS] = {1};
        assert(ps5vk_color_export_format_option(plain, 1) == 0x9u);
        assert(ps5vk_color_export_format_option(blended, 1) == 0x4u);
        /* One nibble per attachment, so two blended targets compose 0x44. */
        {
            const unsigned char two[PS5VK_MAX_COLOR_ATTACHMENTS] = {1, 1};
            assert(ps5vk_color_export_format_option(two, PS5VK_MAX_COLOR_ATTACHMENTS) == 0x44u);
        }
        /* A count the ABI does not carry composes nothing. */
        assert(ps5vk_color_export_format_option(blended, PS5VK_MAX_COLOR_ATTACHMENTS + 1) == 0u);
        assert(ps5vk_color_export_format_option(NULL, 1) == 0u);
    }

    /* SRC1 is the dual-source factor set: it consumes the secondary export and
     * so needs both the enabled feature and the proven export, which the caller
     * checks. Blending disabled never consumes it. */
    assert(!ps5vk_color_attachment_uses_src1(&attachment));
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
    assert(ps5vk_color_attachment_uses_src1(&attachment));
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    assert(!ps5vk_color_attachment_uses_src1(&attachment));
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    assert(ps5vk_color_attachment_uses_src1(&attachment));
    attachment.blendEnable = VK_FALSE;
    assert(!ps5vk_color_attachment_uses_src1(&attachment));
    assert(!ps5vk_color_attachment_uses_src1(NULL));
    return 0;
}

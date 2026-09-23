/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "dual_source_oracle.h"

static int within(int observed, int expected)
{
    return observed + PS5VK_DUAL_SOURCE_TOLERANCE >= expected &&
        observed <= expected + PS5VK_DUAL_SOURCE_TOLERANCE;
}

static int delta(int left, int right)
{
    return left > right ? left - right : right - left;
}

int ps5vk_dual_source_verdict(const unsigned char control[4],
    const unsigned char candidate[4])
{
    if (!control || !candidate) return 0;
    /* The control is the module's own primary export: blending is disabled, so
     * the only freedom is the UNORM8 conversion of channels that are not
     * exactly representable. */
    if (control[3] != PS5VK_DUAL_SOURCE_CONTROL_A) return 0;
    if (!within(control[0], PS5VK_DUAL_SOURCE_CONTROL_R) ||
        !within(control[1], PS5VK_DUAL_SOURCE_CONTROL_G) ||
        !within(control[2], PS5VK_DUAL_SOURCE_CONTROL_B)) return 0;
    /* The candidate is the blend of the two exports.  It is exact, so anything
     * but the exact bytes means the blender did not consume the secondary
     * export, consumed it with the wrong equation, or consumed a stale one. */
    if (candidate[0] != PS5VK_DUAL_SOURCE_BLEND_R ||
        candidate[1] != PS5VK_DUAL_SOURCE_BLEND_G ||
        candidate[2] != PS5VK_DUAL_SOURCE_BLEND_B ||
        candidate[3] != PS5VK_DUAL_SOURCE_BLEND_A) return 0;
    /* The claim is a difference, not two matches: a renderer that ignored the
     * secondary export would report the primary twice, and the two reads are
     * only allowed to differ through the blend state. */
    return delta(control[0], candidate[0]) >= PS5VK_DUAL_SOURCE_MIN_DELTA ||
        delta(control[1], candidate[1]) >= PS5VK_DUAL_SOURCE_MIN_DELTA ||
        delta(control[2], candidate[2]) >= PS5VK_DUAL_SOURCE_MIN_DELTA;
}

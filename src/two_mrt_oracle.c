/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "two_mrt_oracle.h"

static int within(int observed, int expected)
{
    return observed + PS5VK_TWO_MRT_TOLERANCE >= expected &&
        observed <= expected + PS5VK_TWO_MRT_TOLERANCE;
}

static int delta(int left, int right)
{
    return left > right ? left - right : right - left;
}

int ps5vk_two_mrt_verdict(const unsigned char target0[4],
    const unsigned char target1[4])
{
    if (!target0 || !target1) return 0;
    /* Attachment zero is the module's Location 0 export. */
    if (target0[3] != PS5VK_TWO_MRT_TARGET0_A) return 0;
    if (!within(target0[0], PS5VK_TWO_MRT_TARGET0_R) ||
        !within(target0[1], PS5VK_TWO_MRT_TARGET0_G) ||
        !within(target0[2], PS5VK_TWO_MRT_TARGET0_B)) return 0;
    /* Attachment one is the Location 1 export: its colour channels are exact,
     * so anything else means the second target received the first export, a
     * stale value, or nothing at all. */
    if (target1[0] != PS5VK_TWO_MRT_TARGET1_R ||
        target1[1] != PS5VK_TWO_MRT_TARGET1_G ||
        target1[2] != PS5VK_TWO_MRT_TARGET1_B) return 0;
    if (!within(target1[3], PS5VK_TWO_MRT_TARGET1_A)) return 0;
    /* The claim is two distinct exports, not two matches. */
    return delta(target0[0], target1[0]) >= PS5VK_TWO_MRT_MIN_DELTA ||
        delta(target0[1], target1[1]) >= PS5VK_TWO_MRT_MIN_DELTA ||
        delta(target0[2], target1[2]) >= PS5VK_TWO_MRT_MIN_DELTA;
}

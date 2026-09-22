/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "two_mrt_oracle.h"
#include <assert.h>
#include <stddef.h>

int main(void)
{
    const unsigned char target0[4] = {64, 128, 191, 255};
    const unsigned char target1[4] = {204, 102, 51, 128};
    /* The exact pair the witness is built to produce. */
    assert(ps5vk_two_mrt_verdict(target0, target1));
    /* The control side's channels are not exactly representable, and the second
     * attachment's alpha is a half, so one LSB either way is the same freedom
     * the dual-source oracle allows. */
    { const unsigned char low[4] = {63, 127, 191, 255};
      assert(ps5vk_two_mrt_verdict(low, target1)); }
    { const unsigned char high[4] = {65, 129, 192, 255};
      assert(ps5vk_two_mrt_verdict(high, target1)); }
    { const unsigned char alpha[4] = {204, 102, 51, 127};
      assert(ps5vk_two_mrt_verdict(target0, alpha)); }
    { const unsigned char alpha[4] = {204, 102, 51, 129};
      assert(ps5vk_two_mrt_verdict(target0, alpha)); }
    /* The second target's colour channels are exact, so they never move. */
    { const unsigned char off[4] = {205, 102, 51, 128};
      assert(!ps5vk_two_mrt_verdict(target0, off)); }
    { const unsigned char off[4] = {204, 103, 51, 128};
      assert(!ps5vk_two_mrt_verdict(target0, off)); }
    { const unsigned char off[4] = {204, 102, 52, 128};
      assert(!ps5vk_two_mrt_verdict(target0, off)); }
    { const unsigned char off[4] = {204, 102, 51, 126};
      assert(!ps5vk_two_mrt_verdict(target0, off)); }
    /* Neither target may ignore its alpha channel. */
    { const unsigned char bad[4] = {64, 128, 191, 254};
      assert(!ps5vk_two_mrt_verdict(bad, target1)); }
    /* A pipeline that dropped the second export, or wrote the first export into
     * both attachments, reports the same bytes twice. */
    assert(!ps5vk_two_mrt_verdict(target0, target0));
    assert(!ps5vk_two_mrt_verdict(target1, target1));
    /* A target that was never written keeps the clear colour. */
    { const unsigned char clear[4] = {32, 64, 128, 64};
      assert(!ps5vk_two_mrt_verdict(target0, clear)); }
    assert(ps5vk_two_mrt_verdict(NULL, target1) == 0);
    assert(ps5vk_two_mrt_verdict(target0, NULL) == 0);
    assert(ps5vk_two_mrt_verdict(NULL, NULL) == 0);
    return 0;
}

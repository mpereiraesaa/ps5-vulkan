/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "dual_source_oracle.h"
#include <assert.h>
#include <stddef.h>

int main(void)
{
    const unsigned char control[4] = {64, 128, 191, 255};
    const unsigned char candidate[4] = {51, 51, 38, 255};
    /* The exact pair the witness is built to produce. */
    assert(ps5vk_dual_source_verdict(control, candidate));
    /* The control channels are not exactly representable, so the conversion may
     * land one LSB either side; the candidate never may. */
    { const unsigned char low[4] = {63, 127, 191, 255};
      assert(ps5vk_dual_source_verdict(low, candidate)); }
    { const unsigned char high[4] = {65, 129, 192, 255};
      assert(ps5vk_dual_source_verdict(high, candidate)); }
    { const unsigned char off[4] = {52, 51, 38, 255};
      assert(!ps5vk_dual_source_verdict(control, off)); }
    { const unsigned char off[4] = {51, 51, 39, 255};
      assert(!ps5vk_dual_source_verdict(control, off)); }
    /* A blender that ignored the secondary export reports the primary twice. */
    { const unsigned char same[4] = {64, 128, 191, 255};
      assert(!ps5vk_dual_source_verdict(control, same)); }
    /* Neither read may ignore its alpha channel. */
    { const unsigned char bad_control[4] = {64, 128, 191, 254};
      assert(!ps5vk_dual_source_verdict(bad_control, candidate)); }
    { const unsigned char bad_candidate[4] = {51, 51, 38, 128};
      assert(!ps5vk_dual_source_verdict(control, bad_candidate)); }
    /* The blend must move a channel further than the tolerance could explain. */
    assert(ps5vk_dual_source_verdict(NULL, candidate) == 0);
    assert(ps5vk_dual_source_verdict(control, NULL) == 0);
    assert(ps5vk_dual_source_verdict(NULL, NULL) == 0);
    return 0;
}

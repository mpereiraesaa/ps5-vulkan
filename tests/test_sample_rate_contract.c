/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sample_rate_contract.h"
#include <assert.h>

int main(void)
{
    /* The envelope this profile is built for: the single-sample baseline every
     * device serves, plus 2x and 4x. 8x and above are not claimed, and the
     * counts are flags, not numbers, so the mask is what every caller tests
     * against. */
    const VkSampleCountFlags mask = ps5vk_sample_count_mask();
    assert((mask & VK_SAMPLE_COUNT_1_BIT) && (mask & VK_SAMPLE_COUNT_2_BIT) &&
           (mask & VK_SAMPLE_COUNT_4_BIT));
    assert(!(mask & VK_SAMPLE_COUNT_8_BIT) && !(mask & VK_SAMPLE_COUNT_16_BIT) &&
           !(mask & VK_SAMPLE_COUNT_32_BIT) && !(mask & VK_SAMPLE_COUNT_64_BIT));

    /* The number of samples a flag names, and zero for anything this profile
     * does not implement: that zero is what makes an unimplemented count fail
     * closed in every consumer instead of reading as "one". */
    assert(ps5vk_sample_count_number(VK_SAMPLE_COUNT_1_BIT) == 1u);
    assert(ps5vk_sample_count_number(VK_SAMPLE_COUNT_2_BIT) == 2u);
    assert(ps5vk_sample_count_number(VK_SAMPLE_COUNT_4_BIT) == 4u);
    assert(ps5vk_sample_count_number(VK_SAMPLE_COUNT_8_BIT) == 0u);
    assert(ps5vk_sample_count_number((VkSampleCountFlagBits)0) == 0u);
    assert(ps5vk_sample_count_implemented(VK_SAMPLE_COUNT_1_BIT));
    assert(ps5vk_sample_count_implemented(VK_SAMPLE_COUNT_4_BIT));
    assert(!ps5vk_sample_count_implemented(VK_SAMPLE_COUNT_8_BIT));

    /* CB_COLOR0_ATTRIB carries log2 of the count, not the count: the pinned
     * Mesa source writes S_028C74_NUM_SAMPLES(util_logbase2(num_samples)). The
     * register's field positions are pinned by
     * tests/test_sample_count_registers.py against the same table. */
    assert(ps5vk_sample_count_log2(VK_SAMPLE_COUNT_1_BIT) == 0u);
    assert(ps5vk_sample_count_log2(VK_SAMPLE_COUNT_2_BIT) == 1u);
    assert(ps5vk_sample_count_log2(VK_SAMPLE_COUNT_4_BIT) == 2u);

    /* The full mask of a count is the only mask shape the pipeline contract
     * accepts besides an absent one, so it has to cover exactly that many
     * samples - never more, which would name samples the target has not. */
    assert(ps5vk_sample_count_full_mask(VK_SAMPLE_COUNT_1_BIT) == 1u);
    assert(ps5vk_sample_count_full_mask(VK_SAMPLE_COUNT_2_BIT) == 3u);
    assert(ps5vk_sample_count_full_mask(VK_SAMPLE_COUNT_4_BIT) == 15u);
    assert(ps5vk_sample_count_full_mask(VK_SAMPLE_COUNT_8_BIT) == 0u);
    return 0;
}

/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sample_rate_oracle.h"
#include <assert.h>

int main(void)
{
    /* The expected shaded values are the ones the measured run reported: red =
     * sample/255 with alpha one, in the BGRA8 word order the target uses. */
    uint32_t values[8] = {0};
    ps5vk_sample_rate_expected_values(4u, values);
    assert(values[0] == UINT32_C(0xff000000));
    assert(values[1] == UINT32_C(0xff010000));
    assert(values[2] == UINT32_C(0xff020000));
    assert(values[3] == UINT32_C(0xff030000));

    /* Clear phase: the whole span, one distinct value, that value. */
    const uint32_t clear_word = UINT32_C(0xff4080bf);
    assert(ps5vk_sample_rate_clear_verdict(65536u, 65536u, 1u, clear_word,
        clear_word, clear_word));
    /* A single word left uncovered, an extra value, or a different word is not
     * a whole-surface clear. */
    assert(!ps5vk_sample_rate_clear_verdict(65536u, 65535u, 1u, clear_word,
        clear_word, clear_word));
    assert(!ps5vk_sample_rate_clear_verdict(65536u, 65536u, 2u, clear_word,
        clear_word, clear_word));
    assert(!ps5vk_sample_rate_clear_verdict(65536u, 65536u, 1u, clear_word,
        clear_word, UINT32_C(0xff000000)));
    assert(!ps5vk_sample_rate_clear_verdict(0u, 0u, 1u, clear_word, clear_word,
        clear_word));

    /* Shaded phase: one value per sample, all of them the expected ones, over
     * a covered region. */
    assert(ps5vk_sample_rate_shaded_verdict(4u, 4u, 4u, 16384u, values, 4u));
    /* A once-per-pixel draw leaves one value, which is exactly the shape the
     * oracle must refuse. */
    assert(!ps5vk_sample_rate_shaded_verdict(4u, 1u, 1u, 16384u, values, 4u));
    /* A missing or extra value is refused even when the count matches. */
    uint32_t wrong[4] = {UINT32_C(0xff000000), UINT32_C(0xff010000),
                         UINT32_C(0xff020000), UINT32_C(0xff040000)};
    assert(!ps5vk_sample_rate_shaded_verdict(4u, 4u, 4u, 16384u, wrong, 4u));
    assert(!ps5vk_sample_rate_shaded_verdict(4u, 4u, 3u, 16384u, values, 4u));
    /* An empty covered region cannot pass, and neither can a count outside the
     * envelope the contract serves. */
    assert(!ps5vk_sample_rate_shaded_verdict(4u, 4u, 4u, 0u, values, 4u));
    assert(!ps5vk_sample_rate_shaded_verdict(16u, 16u, 16u, 16384u, values, 4u));
    assert(!ps5vk_sample_rate_shaded_verdict(2u, 2u, 2u, 16384u, values, 1u));

    /* Storage: the span is the sample count times the single-sample footprint. */
    assert(ps5vk_sample_rate_storage_ok(4u, 262144u, 65536u));
    assert(ps5vk_sample_rate_storage_ok(2u, 131072u, 65536u));
    assert(!ps5vk_sample_rate_storage_ok(4u, 65536u, 65536u));
    assert(!ps5vk_sample_rate_storage_ok(0u, 262144u, 65536u));
    assert(!ps5vk_sample_rate_storage_ok(4u, 262144u, 0u));
    return 0;
}

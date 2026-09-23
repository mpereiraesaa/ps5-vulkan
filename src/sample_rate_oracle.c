/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sample_rate_oracle.h"

void ps5vk_sample_rate_expected_values(uint32_t samples, uint32_t *out)
{
    if (!out) return;
    for (uint32_t sample = 0; sample < samples; ++sample)
        out[sample] = UINT32_C(0xff000000) | (sample << 16u);
}

int ps5vk_sample_rate_clear_verdict(uint32_t words, uint32_t correct_words,
    uint32_t distinct, uint32_t expected_word, uint32_t first, uint32_t last)
{
    /* One value, that value, in every single word: a span that is not fully
     * covered, or that holds anything besides the clear word, is not the
     * whole-surface clear the profile claims. */
    if (!words || correct_words != words) return 0;
    if (distinct != 1u) return 0;
    if (first != expected_word || last != expected_word) return 0;
    return 1;
}

int ps5vk_sample_rate_shaded_verdict(uint32_t samples, uint32_t shaded_values,
    uint32_t matched_values, uint32_t covered_words, const uint32_t *values,
    uint32_t value_count)
{
    if (!samples || samples > 8u) return 0;
    /* The draw must have covered something, or "no values" would read as a
     * pass on an empty scan. */
    if (!covered_words || !values || value_count < shaded_values) return 0;
    /* Exactly one shaded value per sample, and every one of them the value
     * that sample's own index produces: a per-pixel draw would leave one
     * value, a wrong sample numbering would leave values outside the set. */
    if (shaded_values != samples || matched_values != samples) return 0;
    uint32_t expected[8];
    ps5vk_sample_rate_expected_values(samples, expected);
    for (uint32_t sample = 0; sample < samples; ++sample) {
        int found = 0;
        for (uint32_t value = 0; value < shaded_values && !found; ++value)
            found = values[value] == expected[sample];
        if (!found) return 0;
    }
    return 1;
}

int ps5vk_sample_rate_storage_ok(uint32_t samples, uint64_t span_bytes,
    uint64_t single_sample_bytes)
{
    if (!samples || !single_sample_bytes) return 0;
    if (single_sample_bytes > UINT64_MAX / samples) return 0;
    return span_bytes == single_sample_bytes * samples;
}

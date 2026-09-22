#ifndef PS5VK_SAMPLE_RATE_ORACLE_H
#define PS5VK_SAMPLE_RATE_ORACLE_H

#include <stdint.h>

/* DXVK262-T06 sample-rate oracle. Pure: no Vulkan, no logging, no allocation.
 * The console witness and the host regressions call the same predicates, so the
 * verdict the artifact reports is the one the tests exercise.
 *
 * The witness (native/sample_rate_probe.c) does two things to one multisampled
 * colour target and reads the target's own storage back each time:
 *
 *   clear  - the whole surface is filled with one clear word, so the span must
 *            hold exactly one distinct value, that value, in every word;
 *   shaded - a fragment module that writes gl_SampleID into red with alpha one
 *            is drawn over the whole target, so the span must hold exactly one
 *            distinct shaded value per sample, each of them the value that
 *            sample's index produces. A once-per-pixel draw cannot produce more
 *            than one value at all.
 *
 * The shaded scan deliberately excludes the clear word: a multisampled
 * surface's tiling leaves padding that the clear filled and the draw never
 * touches, so the clear surviving somewhere is expected and is not a shaded
 * value. */

/* The value the fragment module writes for sample `sample` at one sample per
 * invocation: RGBA8 with red = sample/255 and alpha one, which for the BGRA8
 * target is A<<24 | R<<16. `samples` entries are written; the array must hold
 * at least that many. */
void ps5vk_sample_rate_expected_values(uint32_t samples, uint32_t *out);

/* The clear phase's verdict: every word of the span holds the expected clear
 * word, and the scan saw exactly one distinct value. */
int ps5vk_sample_rate_clear_verdict(uint32_t words, uint32_t correct_words,
    uint32_t distinct, uint32_t expected_word, uint32_t first, uint32_t last);

/* The shaded phase's verdict: the scan found exactly one distinct shaded value
 * per sample, all of them the expected ones, and the shaded region was not
 * empty. `values` holds `value_count` distinct shaded values as scanned. */
int ps5vk_sample_rate_shaded_verdict(uint32_t samples, uint32_t shaded_values,
    uint32_t matched_values, uint32_t covered_words, const uint32_t *values,
    uint32_t value_count);

/* The storage relation the profile claims for a multisampled surface: the span
 * is the sample count times the single-sample footprint of the same extent.
 * `single_sample_bytes` is that footprint, which the caller derives from the
 * profile's own layout arithmetic. */
int ps5vk_sample_rate_storage_ok(uint32_t samples, uint64_t span_bytes,
    uint64_t single_sample_bytes);

#endif

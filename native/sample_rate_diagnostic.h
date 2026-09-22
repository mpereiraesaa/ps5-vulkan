#ifndef PS5VK_SAMPLE_RATE_DIAGNOSTIC_H
#define PS5VK_SAMPLE_RATE_DIAGNOSTIC_H
#include <stdint.h>

/* DXVK262-T06 sample-rate line: the ONE channel a diagnostic payload uses to
 * ask the pixel stage a question the shipping path cannot.
 *
 * The pinned min_sample_shading leaves colour every pixel with
 * fract(gl_FragCoord.xy) and require one distinct colour per shaded sample, so
 * the fragment coordinate must be the SAMPLE's and not the pixel's. The
 * compiled state this profile publishes for that module is
 * SPI_PS_INPUT_ENA = SPI_PS_INPUT_ADDR = 0x8380 - the float position AND the
 * fixed-point one, i.e. POS_FIXED_PT_ENA set - and SPI_BARYC_CNTL = 0, whose
 * POS_FLOAT_LOCATION field names the location the float position is
 * interpolated at (0 centre, 1 centroid, 2 sample).
 *
 * Those two registers are the only state that can decide the answer, and the
 * question is which of them the hardware reads. A shipping build has no way to
 * ask it: the values come from the compiler's metadata. This override lets the
 * probe publish other values for the pipelines it creates itself, so ONE
 * payload run measures the whole matrix instead of one console cycle per
 * register. Nothing here is reachable unless PS5VK_SAMPLE_RATE_DIAGNOSTIC is
 * set.
 */

#if PS5VK_SAMPLE_RATE_DIAGNOSTIC
#define PS5VK_SAMPLE_RATE_DIAGNOSTIC_CX_MAX 24
struct ps5vk_sample_rate_diagnostic_cx {
    uint32_t count;
    uint32_t index[PS5VK_SAMPLE_RATE_DIAGNOSTIC_CX_MAX];
    uint32_t value[PS5VK_SAMPLE_RATE_DIAGNOSTIC_CX_MAX];
};
extern struct ps5vk_sample_rate_diagnostic_cx ps5vk_sample_rate_diagnostic_cx;
#endif

#endif

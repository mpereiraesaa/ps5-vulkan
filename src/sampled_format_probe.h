#ifndef PS5VK_SAMPLED_FORMAT_PROBE_H
#define PS5VK_SAMPLED_FORMAT_PROBE_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define PS5VK_SAMPLED_FORMAT_CASES 20u
#define PS5VK_SAMPLED_FORMAT_FILTER_TRIALS 2u

struct ps5vk_sampled_format_case {
    const char *name;
    VkFormat format;
    uint8_t texel[16];
    uint32_t bytes_per_texel;
    uint32_t expected_bgra;
};

int ps5vk_sampled_format_case(unsigned index,
    struct ps5vk_sampled_format_case *out);

/* Exact opaque-black/opaque-white checkerboard texels for a nearest-vs-linear
 * discriminator. The arrays are always 16 bytes; callers consume only the
 * case's bytes_per_texel. */
int ps5vk_sampled_format_filter_texels(unsigned index,
    uint8_t black[16], uint8_t white[16], uint32_t *nearest_bgra,
    uint32_t *linear_bgra);

#endif

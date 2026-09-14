#ifndef PS5VK_SAMPLED_FORMAT_PROBE_H
#define PS5VK_SAMPLED_FORMAT_PROBE_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define PS5VK_SAMPLED_FORMAT_CASES 20u

struct ps5vk_sampled_format_case {
    const char *name;
    VkFormat format;
    uint8_t texel[16];
    uint32_t bytes_per_texel;
    uint32_t expected_bgra;
};

int ps5vk_sampled_format_case(unsigned index,
    struct ps5vk_sampled_format_case *out);

#endif

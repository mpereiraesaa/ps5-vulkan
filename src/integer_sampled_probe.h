#ifndef PS5VK_INTEGER_SAMPLED_PROBE_H
#define PS5VK_INTEGER_SAMPLED_PROBE_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define PS5VK_INTEGER_SAMPLED_CASES_PER_SIGN 9u
#define PS5VK_INTEGER_SAMPLED_EXPECTED_PIXELS 1036800u

struct ps5vk_integer_sampled_case {
    const char *name;
    VkFormat format;
    uint32_t bytes_per_texel;
    uint32_t components;
    uint8_t texel[16];
    uint32_t expected_bgra;
};

int ps5vk_integer_sampled_case(VkBool32 signed_values, unsigned index,
    struct ps5vk_integer_sampled_case *out);

#endif

#ifndef PS5VK_SAMPLER_CORE_PROBE_H
#define PS5VK_SAMPLER_CORE_PROBE_H
#include <stdint.h>
#include <vulkan/vulkan_core.h>
enum { PS5VK_SAMPLER_CORE_CASES = 8 };
struct ps5vk_sampler_core_case {
    const char *name;
    VkSamplerAddressMode address_mode;
    VkBorderColor border_color;
    VkFilter mag_filter;
    VkFilter min_filter;
    float uv;
    uint32_t expected_bgra;
    unsigned checkerboard;
    unsigned minification;
};
int ps5vk_sampler_core_case(unsigned, struct ps5vk_sampler_core_case *);
#endif

#ifndef PS5VK_SAMPLER_CORE_PROBE_H
#define PS5VK_SAMPLER_CORE_PROBE_H
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#define PS5VK_SAMPLER_MIRROR_FIRST_CASE 8
enum { PS5VK_SAMPLER_CORE_CASES = 20 };
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
    /* Zero keeps the older both-axis cases; 1 and 2 select U and V. */
    float uv_v;
    unsigned mirror_axis;
};
int ps5vk_sampler_core_case(unsigned, struct ps5vk_sampler_core_case *);
int ps5vk_sampler_core_color_near(uint32_t actual,uint32_t expected,unsigned tolerance);
#endif

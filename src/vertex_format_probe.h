#ifndef PS5VK_VERTEX_FORMAT_PROBE_H
#define PS5VK_VERTEX_FORMAT_PROBE_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define PS5VK_VERTEX_FORMAT_CASES 11u

enum ps5vk_vertex_probe_numeric {
    PS5VK_VERTEX_PROBE_SINT = 1,
    PS5VK_VERTEX_PROBE_UINT = 2,
    PS5VK_VERTEX_PROBE_UNORM = 3,
};

struct ps5vk_vertex_format_case {
    const char *name;
    VkFormat format;
    uint32_t components;
    enum ps5vk_vertex_probe_numeric numeric;
    uint32_t raw_word;
    float expected[4];
};

int ps5vk_vertex_format_case(unsigned index,
    struct ps5vk_vertex_format_case *out);

#endif

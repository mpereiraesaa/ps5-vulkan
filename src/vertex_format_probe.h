#ifndef PS5VK_VERTEX_FORMAT_PROBE_H
#define PS5VK_VERTEX_FORMAT_PROBE_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define PS5VK_VERTEX_FORMAT_CASES 41u

enum ps5vk_vertex_probe_numeric {
    PS5VK_VERTEX_PROBE_SINT = 1,
    PS5VK_VERTEX_PROBE_UINT = 2,
    PS5VK_VERTEX_PROBE_FLOAT = 3,
};

union ps5vk_vertex_probe_expected {
    float f[4];
    int32_t s[4];
    uint32_t u[4];
};

struct ps5vk_vertex_format_case {
    const char *name;
    VkFormat format;
    uint32_t bytes;
    uint32_t components;
    enum ps5vk_vertex_probe_numeric numeric;
    uint8_t raw[16];
    union ps5vk_vertex_probe_expected expected;
};

int ps5vk_vertex_format_case(unsigned index,
    struct ps5vk_vertex_format_case *out);

#endif

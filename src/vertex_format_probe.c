#include "vertex_format_probe.h"

int ps5vk_vertex_format_case(unsigned index,
    struct ps5vk_vertex_format_case *out)
{
    static const struct ps5vk_vertex_format_case cases[] = {
        {"r32-sint", VK_FORMAT_R32_SINT, 1, PS5VK_VERTEX_PROBE_SINT},
        {"rg32-sint", VK_FORMAT_R32G32_SINT, 2, PS5VK_VERTEX_PROBE_SINT},
        {"rgb32-sint", VK_FORMAT_R32G32B32_SINT, 3, PS5VK_VERTEX_PROBE_SINT},
        {"rgba32-sint", VK_FORMAT_R32G32B32A32_SINT, 4, PS5VK_VERTEX_PROBE_SINT},
        {"r32-uint", VK_FORMAT_R32_UINT, 1, PS5VK_VERTEX_PROBE_UINT},
        {"rg32-uint", VK_FORMAT_R32G32_UINT, 2, PS5VK_VERTEX_PROBE_UINT},
        {"rgb32-uint", VK_FORMAT_R32G32B32_UINT, 3, PS5VK_VERTEX_PROBE_UINT},
        {"rgba32-uint", VK_FORMAT_R32G32B32A32_UINT, 4, PS5VK_VERTEX_PROBE_UINT},
    };
    if (!out || index >= sizeof(cases) / sizeof(cases[0])) return -1;
    *out = cases[index];
    return 0;
}

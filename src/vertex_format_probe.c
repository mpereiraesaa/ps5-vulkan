#include "vertex_format_probe.h"

int ps5vk_vertex_format_case(unsigned index,
    struct ps5vk_vertex_format_case *out)
{
    static const struct ps5vk_vertex_format_case cases[] = {
        {"r32-sint", VK_FORMAT_R32_SINT, 1, PS5VK_VERTEX_PROBE_SINT, UINT32_MAX, {0}},
        {"rg32-sint", VK_FORMAT_R32G32_SINT, 2, PS5VK_VERTEX_PROBE_SINT, UINT32_MAX, {0}},
        {"rgb32-sint", VK_FORMAT_R32G32B32_SINT, 3, PS5VK_VERTEX_PROBE_SINT, UINT32_MAX, {0}},
        {"rgba32-sint", VK_FORMAT_R32G32B32A32_SINT, 4, PS5VK_VERTEX_PROBE_SINT, UINT32_MAX, {0}},
        {"r32-uint", VK_FORMAT_R32_UINT, 1, PS5VK_VERTEX_PROBE_UINT, UINT32_MAX, {0}},
        {"rg32-uint", VK_FORMAT_R32G32_UINT, 2, PS5VK_VERTEX_PROBE_UINT, UINT32_MAX, {0}},
        {"rgb32-uint", VK_FORMAT_R32G32B32_UINT, 3, PS5VK_VERTEX_PROBE_UINT, UINT32_MAX, {0}},
        {"rgba32-uint", VK_FORMAT_R32G32B32A32_UINT, 4, PS5VK_VERTEX_PROBE_UINT, UINT32_MAX, {0}},
        {"rgba8-unorm", VK_FORMAT_R8G8B8A8_UNORM, 4, PS5VK_VERTEX_PROBE_UNORM,
            UINT32_C(0xffaa5511), {17.0f/255.0f,85.0f/255.0f,170.0f/255.0f,1.0f}},
        {"bgra8-unorm", VK_FORMAT_B8G8R8A8_UNORM, 4, PS5VK_VERTEX_PROBE_UNORM,
            UINT32_C(0xffaa5511), {170.0f/255.0f,85.0f/255.0f,17.0f/255.0f,1.0f}},
        {"a2b10g10r10-unorm", VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4,
            PS5VK_VERTEX_PROBE_UNORM, UINT32_C(0xbffaa955),
            {341.0f/1023.0f,682.0f/1023.0f,1.0f,2.0f/3.0f}},
    };
    if (!out || index >= sizeof(cases) / sizeof(cases[0])) return -1;
    *out = cases[index];
    return 0;
}

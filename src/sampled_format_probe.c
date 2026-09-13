#include "sampled_format_probe.h"

int ps5vk_sampled_format_case(unsigned index,
    struct ps5vk_sampled_format_case *out)
{
    static const struct ps5vk_sampled_format_case cases[] = {
        /* Vulkan identity completion must produce (R,0,0,1). */
        {"r8-unorm", VK_FORMAT_R8_UNORM, {0x40,0,0,0}, 1, UINT32_C(0xff400000)},
        /* Vulkan identity completion must produce (R,G,0,1). */
        {"rg8-unorm", VK_FORMAT_R8G8_UNORM, {0x40,0x80,0,0}, 2, UINT32_C(0xff408000)},
        /* 0x80/0x40/0x20 sRGB decode to rounded UNORM8 0x37/0x0d/0x04. */
        {"rgba8-srgb", VK_FORMAT_R8G8B8A8_SRGB, {0x80,0x40,0x20,0xff}, 4,
            UINT32_C(0xff370d04)},
    };
    if (!out || index >= sizeof(cases) / sizeof(cases[0])) return -1;
    *out = cases[index];
    return 0;
}

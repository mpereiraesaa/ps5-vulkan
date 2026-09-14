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
        /* Asymmetric positive values distinguish signed normalization from
         * UNORM while avoiding framebuffer clamping of negative channels. */
        {"r8-snorm", VK_FORMAT_R8_SNORM, {0x40}, 1, UINT32_C(0xff800000)},
        {"rg8-snorm", VK_FORMAT_R8G8_SNORM, {0x40,0x20}, 2, UINT32_C(0xff804000)},
        {"rgba8-snorm", VK_FORMAT_R8G8B8A8_SNORM, {0x20,0x40,0x60,0x7f}, 4,
            UINT32_C(0xff4080c1)},
        /* Shared exponent 15 makes the 9-bit mantissas exact halves:
         * R=256/512, G=128/512 and B=64/512. */
        {"e5b9g9r9-ufloat", VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
            {0x00,0x01,0x01,0x79}, 4, UINT32_C(0xff804020)},
        {"rgba16-sfloat", VK_FORMAT_R16G16B16A16_SFLOAT,
            {0x00,0x38,0x00,0x34,0x00,0x30,0x00,0x3c}, 8,
            UINT32_C(0xff804020)},
        {"rgba32-sfloat", VK_FORMAT_R32G32B32A32_SFLOAT,
            {0x00,0x00,0x00,0x3f,0x00,0x00,0x80,0x3e,
             0x00,0x00,0x00,0x3e,0x00,0x00,0x80,0x3f}, 16,
            UINT32_C(0xff804020)},
        /* Core normalized/float candidates use the Mesa GFX10 format table
         * values reached by ps5-opengl's candidate path.  Missing channels
         * retain Vulkan (0,0,1) completion. */
        {"r16-unorm", VK_FORMAT_R16_UNORM, {0x00,0x80}, 2,
            UINT32_C(0xff800000)},
        {"r16-snorm", VK_FORMAT_R16_SNORM, {0x00,0x40}, 2,
            UINT32_C(0xff800000)},
        {"r16-sfloat", VK_FORMAT_R16_SFLOAT, {0x00,0x38}, 2,
            UINT32_C(0xff800000)},
        {"rg16-unorm", VK_FORMAT_R16G16_UNORM, {0x00,0x80,0x00,0x40}, 4,
            UINT32_C(0xff804000)},
        {"rg16-snorm", VK_FORMAT_R16G16_SNORM, {0x00,0x40,0x00,0x20}, 4,
            UINT32_C(0xff804000)},
        {"rg16-sfloat", VK_FORMAT_R16G16_SFLOAT, {0x00,0x38,0x00,0x34}, 4,
            UINT32_C(0xff804000)},
        {"rgba16-unorm", VK_FORMAT_R16G16B16A16_UNORM,
            {0x00,0x80,0x00,0x40,0x00,0x20,0xff,0xff}, 8,
            UINT32_C(0xff804020)},
        {"rgba16-snorm", VK_FORMAT_R16G16B16A16_SNORM,
            {0x00,0x40,0x00,0x20,0x00,0x10,0xff,0x7f}, 8,
            UINT32_C(0xff804020)},
        {"r32-sfloat", VK_FORMAT_R32_SFLOAT,
            {0x00,0x00,0x00,0x3f}, 4, UINT32_C(0xff800000)},
        {"rg32-sfloat", VK_FORMAT_R32G32_SFLOAT,
            {0x00,0x00,0x00,0x3f,0x00,0x00,0x80,0x3e}, 8,
            UINT32_C(0xff804000)},
        /* R=.5, G=.25, B=.125 in Vulkan B10G11R11 packed order. */
        {"b10g11r11-ufloat", VK_FORMAT_B10G11R11_UFLOAT_PACK32,
            {0x80,0x03,0x1a,0x60}, 4, UINT32_C(0xff804020)},
    };
    _Static_assert(sizeof(cases)/sizeof(cases[0])==PS5VK_SAMPLED_FORMAT_CASES,
        "sampled-format case count");
    if (!out || index >= sizeof(cases) / sizeof(cases[0])) return -1;
    *out = cases[index];
    return 0;
}

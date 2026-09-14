#include "vertex_format_probe.h"

#define RAW_FF {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}
#define SINT32_CASE(n,f,c,e0,e1,e2,e3) \
    {n,f,4u*(c),c,PS5VK_VERTEX_PROBE_SINT,RAW_FF,{.s={e0,e1,e2,e3}}}
#define UINT32_CASE(n,f,c,e0,e1,e2,e3) \
    {n,f,4u*(c),c,PS5VK_VERTEX_PROBE_UINT,RAW_FF,{.u={e0,e1,e2,e3}}}

int ps5vk_vertex_format_case(unsigned index,
    struct ps5vk_vertex_format_case *out)
{
    static const struct ps5vk_vertex_format_case cases[] = {
        SINT32_CASE("r32-sint", VK_FORMAT_R32_SINT, 1, -1,0,0,1),
        SINT32_CASE("rg32-sint", VK_FORMAT_R32G32_SINT, 2, -1,-1,0,1),
        SINT32_CASE("rgb32-sint", VK_FORMAT_R32G32B32_SINT, 3, -1,-1,-1,1),
        SINT32_CASE("rgba32-sint", VK_FORMAT_R32G32B32A32_SINT, 4, -1,-1,-1,-1),
        UINT32_CASE("r32-uint", VK_FORMAT_R32_UINT, 1, UINT32_MAX,0,0,1),
        UINT32_CASE("rg32-uint", VK_FORMAT_R32G32_UINT, 2, UINT32_MAX,UINT32_MAX,0,1),
        UINT32_CASE("rgb32-uint", VK_FORMAT_R32G32B32_UINT, 3, UINT32_MAX,UINT32_MAX,UINT32_MAX,1),
        UINT32_CASE("rgba32-uint", VK_FORMAT_R32G32B32A32_UINT, 4, UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX),
        {"rgba8-unorm",VK_FORMAT_R8G8B8A8_UNORM,4,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x11,0x55,0xaa,0xff},{.f={17.0f/255.0f,85.0f/255.0f,170.0f/255.0f,1}}},
        {"bgra8-unorm",VK_FORMAT_B8G8R8A8_UNORM,4,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x11,0x55,0xaa,0xff},{.f={170.0f/255.0f,85.0f/255.0f,17.0f/255.0f,1}}},
        {"a2b10g10r10-unorm",VK_FORMAT_A2B10G10R10_UNORM_PACK32,4,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x55,0xa9,0xfa,0xbf},{.f={341.0f/1023.0f,682.0f/1023.0f,1,2.0f/3.0f}}},

        {"r8-unorm",VK_FORMAT_R8_UNORM,1,1,PS5VK_VERTEX_PROBE_FLOAT,
            {0x40},{.f={64.0f/255.0f,0,0,1}}},
        {"r8-snorm",VK_FORMAT_R8_SNORM,1,1,PS5VK_VERTEX_PROBE_FLOAT,
            {0x40},{.f={64.0f/127.0f,0,0,1}}},
        {"r8-uint",VK_FORMAT_R8_UINT,1,1,PS5VK_VERTEX_PROBE_UINT,
            {17},{.u={17,0,0,1}}},
        {"r8-sint",VK_FORMAT_R8_SINT,1,1,PS5VK_VERTEX_PROBE_SINT,
            {0xf1},{.s={-15,0,0,1}}},
        {"rg8-unorm",VK_FORMAT_R8G8_UNORM,2,2,PS5VK_VERTEX_PROBE_FLOAT,
            {0x40,0xc0},{.f={64.0f/255.0f,192.0f/255.0f,0,1}}},
        {"rg8-snorm",VK_FORMAT_R8G8_SNORM,2,2,PS5VK_VERTEX_PROBE_FLOAT,
            {0x40,0xc0},{.f={64.0f/127.0f,-64.0f/127.0f,0,1}}},
        {"rg8-uint",VK_FORMAT_R8G8_UINT,2,2,PS5VK_VERTEX_PROBE_UINT,
            {17,201},{.u={17,201,0,1}}},
        {"rg8-sint",VK_FORMAT_R8G8_SINT,2,2,PS5VK_VERTEX_PROBE_SINT,
            {0xf1,0x51},{.s={-15,81,0,1}}},
        {"rgba8-snorm",VK_FORMAT_R8G8B8A8_SNORM,4,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x40,0xc0,0x20,0xe0},{.f={64.0f/127.0f,-64.0f/127.0f,32.0f/127.0f,-32.0f/127.0f}}},
        {"rgba8-uint",VK_FORMAT_R8G8B8A8_UINT,4,4,PS5VK_VERTEX_PROBE_UINT,
            {17,85,170,241},{.u={17,85,170,241}}},
        {"rgba8-sint",VK_FORMAT_R8G8B8A8_SINT,4,4,PS5VK_VERTEX_PROBE_SINT,
            {0xf1,0x22,0xb3,0x44},{.s={-15,34,-77,68}}},
        {"a8b8g8r8-unorm",VK_FORMAT_A8B8G8R8_UNORM_PACK32,4,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x11,0x55,0xaa,0xff},{.f={17.0f/255.0f,85.0f/255.0f,170.0f/255.0f,1}}},
        {"a8b8g8r8-snorm",VK_FORMAT_A8B8G8R8_SNORM_PACK32,4,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x40,0xc0,0x20,0xe0},{.f={64.0f/127.0f,-64.0f/127.0f,32.0f/127.0f,-32.0f/127.0f}}},
        {"a8b8g8r8-uint",VK_FORMAT_A8B8G8R8_UINT_PACK32,4,4,PS5VK_VERTEX_PROBE_UINT,
            {17,85,170,241},{.u={17,85,170,241}}},
        {"a8b8g8r8-sint",VK_FORMAT_A8B8G8R8_SINT_PACK32,4,4,PS5VK_VERTEX_PROBE_SINT,
            {0xf1,0x22,0xb3,0x44},{.s={-15,34,-77,68}}},

        {"r16-unorm",VK_FORMAT_R16_UNORM,2,1,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x40},{.f={16384.0f/65535.0f,0,0,1}}},
        {"r16-snorm",VK_FORMAT_R16_SNORM,2,1,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x40},{.f={16384.0f/32767.0f,0,0,1}}},
        {"r16-uint",VK_FORMAT_R16_UINT,2,1,PS5VK_VERTEX_PROBE_UINT,
            {0x34,0x12},{.u={0x1234,0,0,1}}},
        {"r16-sint",VK_FORMAT_R16_SINT,2,1,PS5VK_VERTEX_PROBE_SINT,
            {0x85,0xff},{.s={-123,0,0,1}}},
        {"r16-sfloat",VK_FORMAT_R16_SFLOAT,2,1,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x38},{.f={0.5f,0,0,1}}},
        {"rg16-unorm",VK_FORMAT_R16G16_UNORM,4,2,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x40,0x00,0xc0},{.f={16384.0f/65535.0f,49152.0f/65535.0f,0,1}}},
        {"rg16-snorm",VK_FORMAT_R16G16_SNORM,4,2,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x40,0x00,0xc0},{.f={16384.0f/32767.0f,-16384.0f/32767.0f,0,1}}},
        {"rg16-uint",VK_FORMAT_R16G16_UINT,4,2,PS5VK_VERTEX_PROBE_UINT,
            {0x34,0x12,0xef,0xcd},{.u={0x1234,0xcdef,0,1}}},
        {"rg16-sint",VK_FORMAT_R16G16_SINT,4,2,PS5VK_VERTEX_PROBE_SINT,
            {0x85,0xff,0xd2,0x04},{.s={-123,1234,0,1}}},
        {"rg16-sfloat",VK_FORMAT_R16G16_SFLOAT,4,2,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x38,0x00,0xbc},{.f={0.5f,-1,0,1}}},
        {"rgba16-unorm",VK_FORMAT_R16G16B16A16_UNORM,8,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x10,0x00,0x40,0x00,0x80,0xff,0xff},
            {.f={4096.0f/65535.0f,16384.0f/65535.0f,32768.0f/65535.0f,1}}},
        {"rgba16-snorm",VK_FORMAT_R16G16B16A16_SNORM,8,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x40,0x00,0xc0,0x00,0x20,0x00,0xe0},
            {.f={16384.0f/32767.0f,-16384.0f/32767.0f,8192.0f/32767.0f,-8192.0f/32767.0f}}},
        {"rgba16-uint",VK_FORMAT_R16G16B16A16_UINT,8,4,PS5VK_VERTEX_PROBE_UINT,
            {0x34,0x12,0xef,0xcd,0x67,0x45,0xab,0x89},{.u={0x1234,0xcdef,0x4567,0x89ab}}},
        {"rgba16-sint",VK_FORMAT_R16G16B16A16_SINT,8,4,PS5VK_VERTEX_PROBE_SINT,
            {0x85,0xff,0xd2,0x04,0xfe,0xff,0x29,0x09},{.s={-123,1234,-2,2345}}},
        {"rgba16-sfloat",VK_FORMAT_R16G16B16A16_SFLOAT,8,4,PS5VK_VERTEX_PROBE_FLOAT,
            {0x00,0x38,0x00,0xbc,0x00,0x41,0x00,0x34},{.f={0.5f,-1,2.5f,0.25f}}},
    };
    _Static_assert(sizeof(cases)/sizeof(cases[0])==PS5VK_VERTEX_FORMAT_CASES,
        "vertex-format case count");
    if (!out || index >= sizeof(cases) / sizeof(cases[0])) return -1;
    *out = cases[index];
    return 0;
}

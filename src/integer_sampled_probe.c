#include "integer_sampled_probe.h"

int ps5vk_integer_sampled_case(VkBool32 signed_values, unsigned index,
    struct ps5vk_integer_sampled_case *out)
{
    static const struct ps5vk_integer_sampled_case unsigned_cases[] = {
        {"r8-uint",VK_FORMAT_R8_UINT,1,1,{51},UINT32_C(0xff330000)},
        {"rg8-uint",VK_FORMAT_R8G8_UINT,2,2,{51,102},UINT32_C(0xff336600)},
        {"rgba8-uint",VK_FORMAT_R8G8B8A8_UINT,4,4,{51,102,153,255},UINT32_C(0xff336699)},
        {"r16-uint",VK_FORMAT_R16_UINT,2,1,{51,0},UINT32_C(0xff330000)},
        {"rg16-uint",VK_FORMAT_R16G16_UINT,4,2,{51,0,102,0},UINT32_C(0xff336600)},
        {"rgba16-uint",VK_FORMAT_R16G16B16A16_UINT,8,4,{51,0,102,0,153,0,255,0},UINT32_C(0xff336699)},
        {"r32-uint",VK_FORMAT_R32_UINT,4,1,{51,0,0,0},UINT32_C(0xff330000)},
        {"rg32-uint",VK_FORMAT_R32G32_UINT,8,2,{51,0,0,0,102,0,0,0},UINT32_C(0xff336600)},
        {"rgba32-uint",VK_FORMAT_R32G32B32A32_UINT,16,4,{51,0,0,0,102,0,0,0,153,0,0,0,255,0,0,0},UINT32_C(0xff336699)},
        {"abgr8-uint-packed",VK_FORMAT_A8B8G8R8_UINT_PACK32,4,4,{51,102,153,255},UINT32_C(0xff336699)},
    };
    static const struct ps5vk_integer_sampled_case signed_cases[] = {
        {"r8-sint",VK_FORMAT_R8_SINT,1,1,{0xc0},UINT32_C(0xff408080)},
        {"rg8-sint",VK_FORMAT_R8G8_SINT,2,2,{0xc0,32},UINT32_C(0xff40a080)},
        {"rgba8-sint",VK_FORMAT_R8G8B8A8_SINT,4,4,{0xc0,32,96,127},UINT32_C(0xff40a0e0)},
        {"r16-sint",VK_FORMAT_R16_SINT,2,1,{0xc0,0xff},UINT32_C(0xff408080)},
        {"rg16-sint",VK_FORMAT_R16G16_SINT,4,2,{0xc0,0xff,32,0},UINT32_C(0xff40a080)},
        {"rgba16-sint",VK_FORMAT_R16G16B16A16_SINT,8,4,{0xc0,0xff,32,0,96,0,127,0},UINT32_C(0xff40a0e0)},
        {"r32-sint",VK_FORMAT_R32_SINT,4,1,{0xc0,0xff,0xff,0xff},UINT32_C(0xff408080)},
        {"rg32-sint",VK_FORMAT_R32G32_SINT,8,2,{0xc0,0xff,0xff,0xff,32,0,0,0},UINT32_C(0xff40a080)},
        {"rgba32-sint",VK_FORMAT_R32G32B32A32_SINT,16,4,{0xc0,0xff,0xff,0xff,32,0,0,0,96,0,0,0,127,0,0,0},UINT32_C(0xff40a0e0)},
        {"abgr8-sint-packed",VK_FORMAT_A8B8G8R8_SINT_PACK32,4,4,{0xc0,32,96,127},UINT32_C(0xff40a0e0)},
    };
    _Static_assert(sizeof(unsigned_cases)/sizeof(unsigned_cases[0]) ==
        PS5VK_INTEGER_SAMPLED_CASES_PER_SIGN, "unsigned sampled case count");
    _Static_assert(sizeof(signed_cases)/sizeof(signed_cases[0]) ==
        PS5VK_INTEGER_SAMPLED_CASES_PER_SIGN, "signed sampled case count");
    if(!out || index>=PS5VK_INTEGER_SAMPLED_CASES_PER_SIGN)return -1;
    *out=(signed_values?signed_cases:unsigned_cases)[index];
    return 0;
}

#include "sampled_format_probe.h"
#include <assert.h>

int main(void)
{
    struct ps5vk_sampled_format_case c;
    assert(!ps5vk_sampled_format_case(0,&c));
    assert(c.format==VK_FORMAT_R8_UNORM && c.bytes_per_texel==1 &&
        c.texel[0]==0x40 && c.expected_bgra==0xff400000u);
    assert(!ps5vk_sampled_format_case(1,&c));
    assert(c.format==VK_FORMAT_R8G8_UNORM && c.bytes_per_texel==2 &&
        c.texel[0]==0x40 && c.texel[1]==0x80 && c.expected_bgra==0xff408000u);
    assert(!ps5vk_sampled_format_case(2,&c));
    assert(c.format==VK_FORMAT_R8G8B8A8_SRGB && c.bytes_per_texel==4 &&
        c.expected_bgra==0xff370d04u);
    assert(!ps5vk_sampled_format_case(3,&c));
    assert(c.format==VK_FORMAT_R8_SNORM && c.bytes_per_texel==1 &&
        c.texel[0]==0x40 && c.expected_bgra==0xff800000u);
    assert(!ps5vk_sampled_format_case(6,&c));
    assert(c.format==VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && c.bytes_per_texel==4 &&
        c.texel[3]==0x79 && c.expected_bgra==0xff804020u);
    assert(!ps5vk_sampled_format_case(8,&c));
    assert(c.format==VK_FORMAT_R32G32B32A32_SFLOAT && c.bytes_per_texel==16 &&
        c.texel[3]==0x3f && c.texel[15]==0x3f);
    assert(!ps5vk_sampled_format_case(9,&c));
    assert(c.format==VK_FORMAT_R16_UNORM && c.bytes_per_texel==2 &&
        c.texel[1]==0x80 && c.expected_bgra==0xff800000u);
    assert(!ps5vk_sampled_format_case(19,&c));
    assert(c.format==VK_FORMAT_B10G11R11_UFLOAT_PACK32 && c.bytes_per_texel==4 &&
        c.texel[0]==0x80 && c.texel[3]==0x60 && c.expected_bgra==0xff804020u);
    assert(ps5vk_sampled_format_case(PS5VK_SAMPLED_FORMAT_CASES,&c));
    assert(ps5vk_sampled_format_case(0,0));
}

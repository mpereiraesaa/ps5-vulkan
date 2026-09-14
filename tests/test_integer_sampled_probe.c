#include "integer_sampled_probe.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    assert(PS5VK_INTEGER_SAMPLED_EXPECTED_PIXELS==1920u*1080u/2u);
    struct ps5vk_integer_sampled_case c;
    assert(!ps5vk_integer_sampled_case(VK_FALSE,0,&c));
    assert(!strcmp(c.name,"r8-uint") && c.format==VK_FORMAT_R8_UINT &&
        c.bytes_per_texel==1 && c.components==1 && c.texel[0]==51 &&
        c.expected_bgra==UINT32_C(0xff330000));
    assert(!ps5vk_integer_sampled_case(VK_FALSE,8,&c));
    assert(c.format==VK_FORMAT_R32G32B32A32_UINT && c.bytes_per_texel==16 &&
        c.components==4 && c.texel[0]==51 && c.texel[4]==102 &&
        c.texel[8]==153 && c.texel[12]==255 &&
        c.expected_bgra==UINT32_C(0xff336699));
    assert(!ps5vk_integer_sampled_case(VK_TRUE,0,&c));
    assert(!strcmp(c.name,"r8-sint") && c.format==VK_FORMAT_R8_SINT &&
        c.texel[0]==0xc0 && c.expected_bgra==UINT32_C(0xff408080));
    assert(!ps5vk_integer_sampled_case(VK_TRUE,8,&c));
    assert(c.format==VK_FORMAT_R32G32B32A32_SINT && c.bytes_per_texel==16 &&
        c.texel[0]==0xc0 && c.texel[1]==0xff && c.texel[4]==32 &&
        c.texel[8]==96 && c.texel[12]==127 &&
        c.expected_bgra==UINT32_C(0xff40a0e0));
    assert(ps5vk_integer_sampled_case(VK_FALSE,
        PS5VK_INTEGER_SAMPLED_CASES_PER_SIGN,&c));
    assert(!ps5vk_integer_sampled_case(VK_FALSE,9,&c));
    assert(c.format == VK_FORMAT_A8B8G8R8_UINT_PACK32 &&
        c.bytes_per_texel == 4 && c.components == 4 &&
        !memcmp(c.texel,(uint8_t[]){51,102,153,255},4) &&
        c.expected_bgra == UINT32_C(0xff336699));
    assert(!ps5vk_integer_sampled_case(VK_TRUE,9,&c));
    assert(c.format == VK_FORMAT_A8B8G8R8_SINT_PACK32 &&
        c.bytes_per_texel == 4 && c.components == 4 &&
        !memcmp(c.texel,(uint8_t[]){0xc0,32,96,127},4) &&
        c.expected_bgra == UINT32_C(0xff40a0e0));
    assert(ps5vk_integer_sampled_case(VK_FALSE,0,NULL));
}

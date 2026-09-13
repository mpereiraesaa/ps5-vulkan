#include "texture_format.h"
#include <assert.h>

int main(void)
{
    const struct ps5vk_texture_format *r=ps5vk_texture_format_lookup(VK_FORMAT_R8_UNORM);
    const struct ps5vk_texture_format *rg=ps5vk_texture_format_lookup(VK_FORMAT_R8G8_UNORM);
    const struct ps5vk_texture_format *rgba=ps5vk_texture_format_lookup(VK_FORMAT_R8G8B8A8_UNORM);
    const struct ps5vk_texture_format *srgb=ps5vk_texture_format_lookup(VK_FORMAT_R8G8B8A8_SRGB);
    assert(r && r->bytes_per_texel==1 && r->descriptor_format_word==0x00100000u);
    assert(r->selectors[0]==4 && r->selectors[1]==0 && r->selectors[2]==0 && r->selectors[3]==1);
    assert(rg && rg->bytes_per_texel==2 && rg->descriptor_format_word==0x00e00000u);
    assert(rg->selectors[0]==4 && rg->selectors[1]==5 && rg->selectors[2]==0 && rg->selectors[3]==1);
    assert(rgba && rgba->bytes_per_texel==4 && rgba->descriptor_format_word==0x03800000u);
    assert(srgb && srgb->bytes_per_texel==4 && srgb->descriptor_format_word==0x08200000u);
    assert(rgba->validated && !r->validated && !rg->validated && !srgb->validated);
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8G8B8A8_UNORM));
    assert(!ps5vk_texture_format_supported(VK_FORMAT_R8_UNORM));
    assert(!ps5vk_texture_format_supported(VK_FORMAT_R8G8_UNORM));
    assert(!ps5vk_texture_format_supported(VK_FORMAT_R8G8B8A8_SRGB));
    assert(!ps5vk_texture_format_lookup(VK_FORMAT_B8G8R8A8_UNORM));
}

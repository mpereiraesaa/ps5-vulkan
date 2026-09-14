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
    assert(r->validated && rg->validated && rgba->validated && srgb->validated);
    assert(!r->linear_filter_validated && !rg->linear_filter_validated &&
        rgba->linear_filter_validated && !srgb->linear_filter_validated);
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8G8B8A8_UNORM));
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8_UNORM));
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8G8_UNORM));
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8G8B8A8_SRGB));
    const VkFormat added[]={VK_FORMAT_R8_SNORM,VK_FORMAT_R8G8_SNORM,
        VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
        VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT};
    const uint32_t words[]={0x00200000u,0x00f00000u,0x03900000u,
        0x08400000u,0x04700000u,0x04d00000u};
    const unsigned sizes[]={1,2,4,4,8,16};
    for(unsigned i=0;i<sizeof(added)/sizeof(added[0]);++i) {
        const struct ps5vk_texture_format *entry=ps5vk_texture_format_lookup(added[i]);
        assert(entry && entry->validated && !entry->linear_filter_validated &&
            entry->descriptor_format_word==words[i] && entry->bytes_per_texel==sizes[i]);
    }
    const struct ps5vk_texture_format *rgb9e5=
        ps5vk_texture_format_lookup(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32);
    assert(rgb9e5->selectors[0]==4 && rgb9e5->selectors[1]==5 &&
        rgb9e5->selectors[2]==6 && rgb9e5->selectors[3]==1);
    assert(!ps5vk_texture_format_lookup(VK_FORMAT_B8G8R8A8_UNORM));
}

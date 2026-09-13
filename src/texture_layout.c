#include "texture_layout.h"
#include "texture_format.h"
int ps5vk_texture_layout_for_format(VkFormat format,uint32_t width,uint32_t height,
    struct ps5vk_texture_layout *out)
{
    const struct ps5vk_texture_format *entry=ps5vk_texture_format_lookup(format);
    if(!out || !entry || !width || !height || width>16384 || height>16384 ||
       width>UINT32_MAX/entry->bytes_per_texel)return -1;
    uint32_t pitch=(width*entry->bytes_per_texel+255u)&~255u;
    *out=(struct ps5vk_texture_layout){pitch,(uint64_t)pitch*height,256};return 0;
}
int ps5vk_texture_layout(uint32_t width,uint32_t height,struct ps5vk_texture_layout *out)
{
    return ps5vk_texture_layout_for_format(VK_FORMAT_R8G8B8A8_UNORM,width,height,out);
}

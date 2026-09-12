#include "texture_layout.h"
int ps5vk_texture_layout(uint32_t width,uint32_t height,struct ps5vk_texture_layout *out)
{
    if(!out || !width || !height || width>16384 || height>16384)return -1;
    uint32_t pitch=(width*4u+255u)&~255u;
    *out=(struct ps5vk_texture_layout){pitch,(uint64_t)pitch*height,256};return 0;
}

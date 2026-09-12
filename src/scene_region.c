#include "scene_region.h"
int ps5vk_scene_region_scan(const uint32_t *pixels,size_t count,unsigned x,unsigned y,
                           struct ps5vk_scene_region *out)
{
    if(!pixels || !out || x>=15 || y>=8)return -1;
    size_t offset=((size_t)y*15+x)*16384;
    if(offset>count || count-offset<16384)return -1;
    struct ps5vk_scene_region r={0};
    for(size_t i=offset;i<offset+16384;++i) {
        switch(pixels[i]) {
        case 0xff000000: ++r.black;break;
        case 0xffff0000: ++r.red;break;
        case 0xff00ff00: ++r.green;break;
        case 0xff0000ff: ++r.blue;break;
        default: ++r.unexpected;break;
        }
    }
    *out=r;return 0;
}

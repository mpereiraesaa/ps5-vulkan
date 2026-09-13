#include "color_clear.h"
int ps5vk_color_clear_bgra8(const float rgba[4], uint32_t *out)
{
    if (!rgba || !out) return 0;
    uint32_t c[4];
    for (unsigned i=0;i<4;++i) {
        if (!(rgba[i]>=0 && rgba[i]<=1)) return 0;
        c[i]=(uint32_t)(rgba[i]*255.0f+0.5f);
    }
    *out=c[2]|(c[1]<<8)|(c[0]<<16)|(c[3]<<24);
    return 1;
}
int ps5vk_color_clear_rgba8(const float rgba[4], uint32_t *out)
{
    if (!rgba || !out) return 0;
    uint32_t c[4];
    for (unsigned i=0;i<4;++i) {
        if (!(rgba[i]>=0 && rgba[i]<=1)) return 0;
        c[i]=(uint32_t)(rgba[i]*255.0f+0.5f);
    }
    *out=c[0]|(c[1]<<8)|(c[2]<<16)|(c[3]<<24);
    return 1;
}

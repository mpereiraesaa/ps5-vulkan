#include "triangle_readback.h"
struct ps5vk_triangle_readback ps5vk_triangle_scan(const uint32_t *words,size_t count,uint32_t sentinel)
{
    struct ps5vk_triangle_readback s={.minimum={255,255,255}};
    if(!words)return s;
    for(size_t i=0;i<count;++i) {
        uint32_t p=words[i];if(p==sentinel)continue;++s.changed;
        if((p>>24)!=255)++s.bad_alpha;
        unsigned sum=0;
        for(unsigned c=0;c<3;++c) {
            unsigned v=(p>>(8*c))&255;sum+=v;
            if(v<s.minimum[c])s.minimum[c]=v;
            if(v>s.maximum[c])s.maximum[c]=v;
        }
        /* Linear interpolation of RGB basis vectors sums to one. Three UNORM8
         * roundings allow a sum of 254..256, without assuming channel order. */
        if(sum<254 || sum>256)++s.bad_sum;
    }
    return s;
}
int ps5vk_triangle_coverage_valid(const struct ps5vk_triangle_readback *s,uint32_t width,uint32_t height)
{
    if(!s || !width || !height || width>16384 || height>16384 || !s->changed || s->bad_alpha || s->bad_sum)return 0;
    /* Source triangle: base .7*w, height .65*h => .2275*w*h. Edge
     * quantization allowance is a conservative two-pixel perimeter band. */
    uint64_t expected=(uint64_t)width*height*91/400, tolerance=2ull*(width+height);
    uint64_t difference=s->changed>expected ? s->changed-expected : expected-s->changed;
    if(difference>tolerance)return 0;
    return 1;
}
int ps5vk_triangle_readback_valid(const struct ps5vk_triangle_readback *s,uint32_t width,uint32_t height)
{
    if(!ps5vk_triangle_coverage_valid(s,width,height))return 0;
    for(unsigned c=0;c<3;++c)if(s->minimum[c]>10 || s->maximum[c]<240)return 0;
    return 1;
}

#include "triangle_readback.h"
#include <assert.h>
int main(void)
{
    const uint32_t pixels[]={0x55aa11ee,0xffff0000,0xff00ff00,0xff0000ff,0xff555555};
    struct ps5vk_triangle_readback s=ps5vk_triangle_scan(pixels,5,0x55aa11ee);
    assert(s.changed==4 && !s.bad_alpha && !s.bad_sum);
    for(unsigned c=0;c<3;++c)assert(s.minimum[c]==0 && s.maximum[c]==255);
    /* Synthetic statistics only, not a CPU-rendered image. */
    s.changed=471744;assert(ps5vk_triangle_readback_valid(&s,1920,1080));
    assert(!ps5vk_triangle_readback_valid(&s,960,540));
    s.changed=117936;assert(ps5vk_triangle_readback_valid(&s,960,540));
    s.minimum[0]=s.maximum[0]=255;s.minimum[1]=s.maximum[1]=0;s.minimum[2]=s.maximum[2]=0;
    assert(ps5vk_triangle_coverage_valid(&s,960,540));
    assert(!ps5vk_triangle_readback_valid(&s,960,540));
    s.bad_sum=1;assert(!ps5vk_triangle_readback_valid(&s,960,540));
    const uint32_t bad[]={0x7fff0000,0xffffffff,0xff000000};
    s=ps5vk_triangle_scan(bad,3,0x55aa11ee);assert(s.bad_alpha==1 && s.bad_sum==2);
    s=ps5vk_triangle_scan(NULL,3,0);assert(!ps5vk_triangle_readback_valid(&s,1920,1080));
}

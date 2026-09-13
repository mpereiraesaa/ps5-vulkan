#include "color_detile.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    const uint32_t width=257,height=129;
    const size_t linear_bytes=(size_t)width*height*4;
    const size_t tiled_bytes=6u*65536u;
    unsigned char *linear=malloc(linear_bytes),*tiled=calloc(1,tiled_bytes),*out=malloc(linear_bytes);
    assert(linear && tiled && out);
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        size_t linear_offset=((size_t)y*width+x)*4;
        uint32_t value=0xff000000u|((y<<12)^x);
        memcpy(linear+linear_offset,&value,4);
        size_t tiled_offset=ps5vk_rgba8_64k_rx_offset(x,y,width);
        assert(tiled_offset+4<=tiled_bytes);
        memcpy(tiled+tiled_offset,&value,4);
    }
    assert(!ps5vk_rgba8_64k_rx_detile(out,linear_bytes,tiled,tiled_bytes,width,height));
    assert(!memcmp(out,linear,linear_bytes));
    assert(ps5vk_rgba8_64k_rx_detile(out,linear_bytes-1,tiled,tiled_bytes,width,height));
    assert(ps5vk_rgba8_64k_rx_detile(out,linear_bytes,tiled,tiled_bytes-65536,width,height));
    assert(ps5vk_rgba8_64k_rx_offset(0,0,128)==0);
    assert(ps5vk_rgba8_64k_rx_offset(127,127,128)<65536);
    assert(ps5vk_rgba8_64k_rx_offset(128,0,256)==65536);
    free(out);free(tiled);free(linear);
    puts("RGBA8 64KB_R_X detile: pass (host only)");
}

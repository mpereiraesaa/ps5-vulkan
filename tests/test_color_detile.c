#include "color_detile.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct layout_case {
    uint32_t bpp;
    uint32_t tile_width;
    uint32_t tile_height;
};

static const struct layout_case layouts[] = {
    {1,256,256}, {2,256,128}, {4,128,128}, {8,128,64}, {16,64,64},
};

static void test_one_tile_is_a_bijection(const struct layout_case *layout)
{
    const size_t slots=65536u/layout->bpp;
    unsigned char *seen=calloc(slots,1);
    assert(seen);
    assert(ps5vk_color_64k_rx_surface_size(layout->bpp,
        layout->tile_width,layout->tile_height)==65536u);
    for(uint32_t y=0;y<layout->tile_height;++y) {
        for(uint32_t x=0;x<layout->tile_width;++x) {
            size_t offset=ps5vk_color_64k_rx_offset(layout->bpp,x,y,
                layout->tile_width);
            assert(offset<65536u);
            assert(offset%layout->bpp==0);
            assert(!seen[offset/layout->bpp]);
            seen[offset/layout->bpp]=1;
        }
    }
    for(size_t slot=0;slot<slots;++slot)assert(seen[slot]);
    free(seen);
}

static void test_multitile_roundtrip(const struct layout_case *layout)
{
    const uint32_t width=layout->tile_width+1;
    const uint32_t height=layout->tile_height+1;
    const size_t linear_bytes=(size_t)width*height*layout->bpp;
    const size_t tiled_bytes=4u*65536u;
    unsigned char *linear=malloc(linear_bytes);
    unsigned char *tiled=calloc(1,tiled_bytes);
    unsigned char *out=malloc(linear_bytes);
    assert(linear&&tiled&&out);
    assert(ps5vk_color_64k_rx_surface_size(layout->bpp,width,height)==
        tiled_bytes);
    for(uint32_t y=0;y<height;++y) {
        for(uint32_t x=0;x<width;++x) {
            size_t linear_offset=((size_t)y*width+x)*layout->bpp;
            size_t tiled_offset=ps5vk_color_64k_rx_offset(layout->bpp,x,y,
                width);
            assert(tiled_offset<=tiled_bytes-layout->bpp);
            for(uint32_t byte=0;byte<layout->bpp;++byte)
                linear[linear_offset+byte]=(unsigned char)(x^(y*3u)^
                    (byte*0x5bu)^layout->bpp);
            memcpy(tiled+tiled_offset,linear+linear_offset,layout->bpp);
        }
    }
    assert(!ps5vk_color_64k_rx_detile(out,linear_bytes,tiled,tiled_bytes,
        layout->bpp,width,height));
    assert(!memcmp(out,linear,linear_bytes));
    assert(ps5vk_color_64k_rx_detile(out,linear_bytes-1,tiled,tiled_bytes,
        layout->bpp,width,height));
    assert(ps5vk_color_64k_rx_detile(out,linear_bytes,tiled,tiled_bytes-1,
        layout->bpp,width,height));
    free(out);free(tiled);free(linear);
}

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
    for(size_t i=0;i<sizeof(layouts)/sizeof(layouts[0]);++i) {
        test_one_tile_is_a_bijection(&layouts[i]);
        test_multitile_roundtrip(&layouts[i]);
    }
    assert(ps5vk_color_64k_rx_surface_size(3,64,64)==SIZE_MAX);
    assert(ps5vk_color_64k_rx_surface_size(4,0,64)==SIZE_MAX);
    assert(ps5vk_color_64k_rx_offset(3,0,0,64)==SIZE_MAX);
    assert(ps5vk_color_64k_rx_offset(4,64,0,64)==SIZE_MAX);
    puts("SW_64K_R_X 1/2/4/8/16-byte detile: pass (host only)");
}

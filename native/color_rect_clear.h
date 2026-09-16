#ifndef PS5VK_COLOR_RECT_CLEAR_H
#define PS5VK_COLOR_RECT_CLEAR_H
#include "color_detile.h"
#include "texture_dma.h"
#include <vulkan/vulkan.h>

/* RGBA8 SW_64K_R_X has contiguous 4x8-texel microtiles. The existing public
 * address equation is authoritative; only full microtiles use a 128-byte fill,
 * and clipped edges use contiguous runs of at most four texels. No CPU writes
 * target pixels. Caller orders CB writeback before DMA and acquires afterwards.
 * Two passes make insufficient capacity/invalid ranges atomic to the caller. */
static inline int ps5vk_rect_fill_range(uint32_t *out,size_t capacity,size_t *used,
    uint64_t base,uint64_t allocation,uint64_t offset,uint64_t bytes,uint32_t word)
{
    const uint64_t chunk=0x1ffffc;
    uint64_t packets=bytes/chunk+(bytes%chunk!=0);
    if(!bytes || bytes%4 || offset%4 || offset>allocation || bytes>allocation-offset ||
       *used>capacity || packets>(capacity-*used)/7)return -1;
    size_t words=(size_t)packets*7;
    if(out && ps5vk_dma_fill(out+*used,capacity-*used,base+offset,bytes,word)!=words)return -1;
    *used+=words;
    return 0;
}

static inline size_t ps5vk_color_rect_clear(uint32_t *out,size_t capacity,
    uint64_t base,uint64_t allocation,size_t stride,uint32_t width,uint32_t height,
    uint32_t layers,uint32_t view_mask,VkRect2D rect,uint32_t word)
{
    const uint64_t limit=UINT64_C(1)<<48;
    const size_t surface=ps5vk_color_64k_rx_surface_size(4,width,height);
    if(!out || !base || base%4 || base>=limit || allocation>limit-base ||
       !layers || layers>32 || width>UINT32_MAX-4u || height>UINT32_MAX-8u ||
       surface==SIZE_MAX || stride<surface || stride%4 ||
       stride>allocation/layers || rect.offset.x<0 || rect.offset.y<0 ||
       !rect.extent.width || !rect.extent.height ||
       (uint32_t)rect.offset.x>width || rect.extent.width>width-(uint32_t)rect.offset.x ||
       (uint32_t)rect.offset.y>height || rect.extent.height>height-(uint32_t)rect.offset.y ||
       (layers<32 && (view_mask>>layers)))return 0;
    const uint32_t mask=view_mask?view_mask:1u;
    const uint32_t x0=(uint32_t)rect.offset.x,y0=(uint32_t)rect.offset.y;
    const uint32_t x1=x0+rect.extent.width,y1=y0+rect.extent.height;
    size_t used=0;
    for(unsigned emit=0;emit<2;++emit) {
        used=0;
        for(uint32_t layer=0;layer<layers;++layer) {
            if(!(mask&(UINT32_C(1)<<layer)))continue;
            const uint64_t layer_base=(uint64_t)layer*stride;
            if(!x0 && !y0 && x1==width && y1==height) {
                if(ps5vk_rect_fill_range(emit?out:NULL,capacity,&used,base,allocation,
                    layer_base,stride,word))return 0;
                continue;
            }
            for(uint32_t by=y0&~7u;by<y1;by+=8)
                for(uint32_t bx=x0&~3u;bx<x1;bx+=4) {
                    const uint32_t left=bx<x0?x0:bx,right=bx+4>x1?x1:bx+4;
                    const uint32_t top=by<y0?y0:by,bottom=by+8>y1?y1:by+8;
                    if(left==bx && right==bx+4 && top==by && bottom==by+8) {
                        size_t offset=ps5vk_rgba8_64k_rx_offset(bx,by,width);
                        if(offset==SIZE_MAX || ps5vk_rect_fill_range(emit?out:NULL,capacity,&used,
                            base,allocation,layer_base+offset,128,word))return 0;
                    } else for(uint32_t y=top;y<bottom;++y) {
                        size_t offset=ps5vk_rgba8_64k_rx_offset(left,y,width);
                        if(offset==SIZE_MAX || ps5vk_rect_fill_range(emit?out:NULL,capacity,&used,
                            base,allocation,layer_base+offset,(right-left)*4u,word))return 0;
                    }
                }
        }
    }
    return used;
}
#endif

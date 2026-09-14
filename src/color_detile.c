/* Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from BlackBearReloaded's ps5-opengl
 * src/gallium/ps5/ps5_screen.c:ps5_tiled_color_offset at commit
 * 7f9bfabdddb187a11e4401058eba8c9e55194d0a (GPL-3.0-or-later).
 * Its coordinate-ramp receipts identify the PS5 SW_64K_R_X/swizzle-27
 * equations for 1, 2, 4, 8 and 16-byte texels. */
#include "color_detile.h"
#include <limits.h>
#include <string.h>

static size_t affine(uint32_t x,uint32_t y,const uint16_t *xm,
    unsigned xb,const uint16_t *ym,unsigned yb)
{
    size_t result=0;
    for(unsigned bit=0;bit<xb;++bit)if(x&(1u<<bit))result^=xm[bit];
    for(unsigned bit=0;bit<yb;++bit)if(y&(1u<<bit))result^=ym[bit];
    return result;
}

static int tile(uint32_t bpp,uint32_t *width,uint32_t *height)
{
    if(!width||!height)return 0;
    switch(bpp) {
    case 1:*width=256;*height=256;return 1;
    case 2:*width=256;*height=128;return 1;
    case 4:*width=128;*height=128;return 1;
    case 8:*width=128;*height=64;return 1;
    case 16:*width=64;*height=64;return 1;
    default:return 0;
    }
}

size_t ps5vk_color_64k_rx_surface_size(uint32_t bpp,uint32_t width,uint32_t height)
{
    uint32_t tw,th;
    if(!width||!height||!tile(bpp,&tw,&th))return SIZE_MAX;
    size_t tx=((size_t)width+tw-1u)/tw,ty=((size_t)height+th-1u)/th;
    if(ty>SIZE_MAX/tx||tx*ty>SIZE_MAX/UINT32_C(0x10000))return SIZE_MAX;
    return tx*ty*UINT32_C(0x10000);
}

size_t ps5vk_color_64k_rx_offset(uint32_t bpp,uint32_t x,uint32_t y,uint32_t width)
{
    static const uint16_t r8x[8]={0x0001,0x0002,0x0004,0x0140,0x0200,0x0800,0x2400,0x8000};
    static const uint16_t r8y[8]={0x0010,0x0008,0x0020,0x0100,0x0280,0x0400,0x1800,0x4000};
    static const uint16_t rg8x[8]={0x0001,0x0002,0x0004,0x00c0,0x0100,0x0400,0x1200,0x4000};
    static const uint16_t rg8y[7]={0x0008,0x0010,0x0020,0x0080,0x0900,0x0200,0x2400};
    static const uint16_t rgba8x[7]={0x0004,0x0008,0x0080,0x0100,0x2200,0x0800,0x8400};
    static const uint16_t rgba8y[7]={0x0010,0x0020,0x0040,0x1100,0x0200,0x0400,0x4800};
    static const uint16_t rgba16x[7]={0x0008,0x0020,0x0040,0x2100,0x0200,0x0800,0x8400};
    static const uint16_t rgba16y[7]={0x0010,0x0080,0x1000,0x0100,0x4200,0x0400,0x0800};
    static const uint16_t rgba32x[7]={0x0010,0x0040,0x2000,0x0100,0x8200,0x0800,0x0400};
    static const uint16_t rgba32y[7]={0x0020,0x0080,0x1000,0x4100,0x0200,0x0400,0x0800};
    uint32_t tw,th;size_t local;
    if(!width||x>=width||!tile(bpp,&tw,&th))return SIZE_MAX;
    switch(bpp) {
    case 1:local=affine(x&255u,y&255u,r8x,8,r8y,8);break;
    case 2:local=affine(x&255u,y&127u,rg8x,8,rg8y,7)<<1;break;
    case 4:local=affine(x&127u,y&127u,rgba8x,7,rgba8y,7);break;
    case 8:local=affine(x,y,rgba16x,7,rgba16y,7);break;
    case 16:local=affine(x,y,rgba32x,7,rgba32y,7);break;
    default:return SIZE_MAX;
    }
    size_t tx=((size_t)width+tw-1u)/tw,tile_y=y/th;
    if(tile_y>SIZE_MAX/tx)return SIZE_MAX;
    size_t index=tile_y*tx+x/tw;
    if(index>SIZE_MAX/UINT32_C(0x10000))return SIZE_MAX;
    return index*UINT32_C(0x10000)+local;
}

int ps5vk_color_64k_rx_detile(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t bpp,uint32_t width,uint32_t height)
{
    size_t surface=ps5vk_color_64k_rx_surface_size(bpp,width,height);
    if(!destination||!source||surface==SIZE_MAX||source_bytes<surface||
        width>SIZE_MAX/bpp||(size_t)width*bpp>SIZE_MAX/height||
        destination_bytes<(size_t)width*height*bpp)return -1;
    unsigned char *dst=destination;const unsigned char *src=source;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        size_t offset=ps5vk_color_64k_rx_offset(bpp,x,y,width);
        if(offset==SIZE_MAX||offset>source_bytes-bpp)return -1;
        memcpy(dst+((size_t)y*width+x)*bpp,src+offset,bpp);
    }
    return 0;
}

size_t ps5vk_rgba8_64k_rx_offset(uint32_t x,uint32_t y,uint32_t width)
{return ps5vk_color_64k_rx_offset(4,x,y,width);}

int ps5vk_rgba8_64k_rx_detile(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t width,uint32_t height)
{return ps5vk_color_64k_rx_detile(destination,destination_bytes,source,source_bytes,4,width,height);}

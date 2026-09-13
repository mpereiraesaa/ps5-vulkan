/* Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The 32-bpp SW_64K_R_X affine equation is derived from BlackBearReloaded's
 * ps5-opengl ps5_tiled_color_offset implementation (GPL-3.0-or-later), whose
 * coordinate-ramp receipt identifies the PS5 swizzle-27 layout. */
#include "color_detile.h"
#include <limits.h>
#include <string.h>

size_t ps5vk_rgba8_64k_rx_offset(uint32_t x,uint32_t y,uint32_t width)
{
    static const uint16_t xm[7]={0x0004,0x0008,0x0080,0x0100,0x2200,0x0800,0x8400};
    static const uint16_t ym[7]={0x0010,0x0020,0x0040,0x1100,0x0200,0x0400,0x4800};
    size_t local=0;
    uint32_t lx=x&127u,ly=y&127u;
    for(unsigned bit=0;bit<7;++bit) {
        if(lx&(1u<<bit))local^=xm[bit];
        if(ly&(1u<<bit))local^=ym[bit];
    }
    return ((((size_t)y>>7)*(((size_t)width+127u)>>7)+(x>>7))<<16)+local;
}

int ps5vk_rgba8_64k_rx_detile(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t width,uint32_t height)
{
    if(!destination || !source || !width || !height || width>UINT32_MAX/4u)return -1;
    const size_t row=(size_t)width*4u;
    if(height>SIZE_MAX/row || destination_bytes<(size_t)height*row)return -1;
    unsigned char *dst=destination;const unsigned char *src=source;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        size_t offset=ps5vk_rgba8_64k_rx_offset(x,y,width);
        if(offset>source_bytes || source_bytes-offset<4)return -1;
        memcpy(dst+(size_t)y*row+(size_t)x*4u,src+offset,4);
    }
    return 0;
}

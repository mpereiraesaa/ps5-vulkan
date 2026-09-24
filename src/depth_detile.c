/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFX10 SW_64K_Z_X pixel addressing for single-sample D32.
 *
 * The equation is the one AMD's address library publishes for this swizzle
 * mode, taken from Mesa's vendored copy (MIT):
 * src/amd/addrlib/src/gfx10/gfx10SwizzlePattern.h,
 * GFX10_SW_64K_Z_X_1xaa_PATINFO, the 16-pipe row for a 4-byte element,
 * assembled from GFX10_SW_PATTERN_NIBBLE01/2/3/4 exactly as
 * Gfx10Lib::GetSwizzlePatternFromPatternInfo does and evaluated as
 * Lib::ComputeOffsetFromSwizzlePattern does.
 *
 * Why the 16-pipe row is the right one for this console is not assumed: the
 * SAME table's SW_64K_R_X rows at 16 pipes reproduce, bit for bit over every
 * coordinate in a tile, all three colour equations this driver already
 * measured on hardware through ps5-opengl's coordinate-ramp receipts (the
 * 1, 2 and 4-byte texel masks in src/color_detile.c). A configuration that
 * reproduces three independently witnessed equations is the configuration
 * this GPU runs, so the depth row of the same table is derived rather than
 * guessed. tests/test_depth_detile.c pins that cross-check.
 *
 * Both equations are affine over GF(2), so the 20-bit pattern is stored here
 * in the same per-coordinate-bit mask form src/color_detile.c uses. */
#include "depth_detile.h"
#include <limits.h>
#include <string.h>

/* SW_64K_Z_X, 1xaa, 16 pipes, 4-byte element. Tile is 128x128 samples. */
#define PS5VK_DEPTH_TILE_WIDTH  128u
#define PS5VK_DEPTH_TILE_HEIGHT 128u

static size_t affine(uint32_t x,uint32_t y,const uint16_t *xm,
    unsigned xb,const uint16_t *ym,unsigned yb)
{
    size_t result=0;
    for(unsigned bit=0;bit<xb;++bit)if(x&(1u<<bit))result^=xm[bit];
    for(unsigned bit=0;bit<yb;++bit)if(y&(1u<<bit))result^=ym[bit];
    return result;
}

size_t ps5vk_depth_64k_zx_surface_size(uint32_t width,uint32_t height)
{
    if(!width||!height)return SIZE_MAX;
    size_t tx=((size_t)width+PS5VK_DEPTH_TILE_WIDTH-1u)/PS5VK_DEPTH_TILE_WIDTH;
    size_t ty=((size_t)height+PS5VK_DEPTH_TILE_HEIGHT-1u)/PS5VK_DEPTH_TILE_HEIGHT;
    if(ty>SIZE_MAX/tx||tx*ty>SIZE_MAX/UINT32_C(0x10000))return SIZE_MAX;
    return tx*ty*UINT32_C(0x10000);
}

size_t ps5vk_depth_64k_zx_offset(uint32_t x,uint32_t y,uint32_t width)
{
    static const uint16_t zx[7]={0x0004,0x0010,0x0040,0x0100,0x2200,0x0800,0x8400};
    static const uint16_t zy[7]={0x0008,0x0020,0x0080,0x1100,0x0200,0x0400,0x4800};
    if(!width||x>=width)return SIZE_MAX;
    size_t local=affine(x&(PS5VK_DEPTH_TILE_WIDTH-1u),y&(PS5VK_DEPTH_TILE_HEIGHT-1u),
                        zx,7,zy,7);
    size_t tx=((size_t)width+PS5VK_DEPTH_TILE_WIDTH-1u)/PS5VK_DEPTH_TILE_WIDTH;
    size_t tile_y=y/PS5VK_DEPTH_TILE_HEIGHT;
    if(tile_y>SIZE_MAX/tx)return SIZE_MAX;
    size_t index=tile_y*tx+x/PS5VK_DEPTH_TILE_WIDTH;
    if(index>SIZE_MAX/UINT32_C(0x10000))return SIZE_MAX;
    return index*UINT32_C(0x10000)+local;
}

size_t ps5vk_depth_64k_zx_gather_mip_offset(uint32_t mip,uint32_t x,uint32_t y)
{
    /* Mesa AddrLib 6.2, Gfx10Lib::ComputeSurfaceInfoMacroTiled, for a
     * 64x64x1, seven-mip, 32bpp 2D 64KB_Z_X chain. All seven levels fit in
     * one 64 KiB mip-tail block. These coordinates are the published
     * mipTailCoordX/Y values, not per-level tile origins. */
    static const uint8_t tail_x[7]={64,0,32,0,16,8,0};
    static const uint8_t tail_y[7]={0,64,0,32,0,16,24};
    if(mip>=7)return SIZE_MAX;
    uint32_t width=64u>>mip,height=64u>>mip;
    if(!width)width=1;
    if(!height)height=1;
    if(x>=width||y>=height)return SIZE_MAX;
    /* The top level of this chain is already in the mip tail. AddrLib
     * addresses every tail mip inside the single 128x128 Z_X macroblock. */
    return ps5vk_depth_64k_zx_offset((uint32_t)tail_x[mip]+x,
        (uint32_t)tail_y[mip]+y,128u);
}

int ps5vk_depth_64k_zx_detile(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t width,uint32_t height)
{
    size_t surface=ps5vk_depth_64k_zx_surface_size(width,height);
    if(!destination||!source||surface==SIZE_MAX||source_bytes<surface||
        !width||!height||(size_t)width*4u>SIZE_MAX/height||
        destination_bytes<(size_t)width*height*4u)return -1;
    unsigned char *dst=destination;const unsigned char *src=source;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        size_t offset=ps5vk_depth_64k_zx_offset(x,y,width);
        if(offset==SIZE_MAX||offset>source_bytes-4u)return -1;
        memcpy(dst+((size_t)y*width+x)*4u,src+offset,4u);
    }
    return 0;
}

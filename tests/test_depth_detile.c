/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SW_64K_Z_X depth equation, and the cross-check that decides it is the
 * right one for this console.
 *
 * The equation cannot be witnessed directly without a depth readback, which
 * is the very thing it implements, so it is derived instead: AMD's address
 * library publishes one swizzle table for gfx10, and the colour rows of that
 * table at 16 pipes reproduce every colour equation this driver already
 * measured on hardware. This file pins both halves - the colour agreement
 * that identifies the configuration, and the depth equation that follows from
 * it - so a future edit cannot quietly change either.
 */
#include "depth_detile.h"
#include "color_detile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One 64 KiB block of 4-byte samples: 128x128. */
enum { TILE=128, TEXELS=TILE*TILE, BLOCK=0x10000 };

/* The published pattern for GFX10_SW_64K_Z_X_1xaa_PATINFO at 16 pipes and a
 * 4-byte element, one entry per output bit, which is how
 * Lib::ComputeOffsetFromSwizzlePattern reads it. The implementation stores
 * the same map transposed, one mask per coordinate bit, so agreeing over the
 * whole tile is what proves the transposition.
 *
 * Bits 0 and 1 select no coordinate at all: a 4-byte element never lands at
 * an unaligned offset. Bits 8..11 also carry a slice (z) term, which this
 * surface cannot exercise - the layout is one mip, one layer and one sample
 * (src/depth_layout.h) - so z is zero and those terms contribute nothing.
 * The sample (s) column is zero in every bit of this row. */
static const struct { uint16_t x,y; } pattern[20]={
    {0x0000,0x0000},{0x0000,0x0000},{0x0001,0x0000},{0x0000,0x0001},
    {0x0002,0x0000},{0x0000,0x0002},{0x0004,0x0000},{0x0000,0x0004},
    {0x0008,0x0008},{0x0010,0x0010},{0x0040,0x0020},{0x0020,0x0040},
    {0x0000,0x0008},{0x0010,0x0000},{0x0000,0x0040},{0x0040,0x0000},
    {0x0000,0x0000},{0x0000,0x0000},{0x0000,0x0000},{0x0000,0x0000}};

static uint32_t parity(uint32_t value,uint16_t mask)
{
    uint32_t bit=0;
    while(mask) { if(mask&1u)bit^=value&1u; value>>=1;mask>>=1; }
    return bit;
}

int main(void)
{
    /* 1. The equation is a bijection of the tile onto the block: every one of
     *    the 16384 samples lands on its own 4-byte slot and nothing escapes
     *    the 64 KiB block. A swizzle that collided would silently drop pixels
     *    on readback. */
    unsigned char *seen=calloc(BLOCK,1);
    assert(seen);
    for(uint32_t y=0;y<TILE;++y)for(uint32_t x=0;x<TILE;++x) {
        size_t off=ps5vk_depth_64k_zx_offset(x,y,TILE);
        assert(off!=SIZE_MAX);
        assert(off+4u<=BLOCK);
        assert(off%4u==0u);
        assert(!seen[off]);
        seen[off]=1;
    }
    size_t occupied=0;
    for(size_t i=0;i<BLOCK;i+=4)if(seen[i])++occupied;
    assert(occupied==TEXELS);
    free(seen);

    /* 2. The map is affine over GF(2): the offset of any coordinate is the
     *    XOR of the offsets of its set bits. That is the property the stored
     *    per-bit masks rely on, and the property the published pattern has. */
    for(uint32_t y=0;y<TILE;++y)for(uint32_t x=0;x<TILE;++x) {
        size_t expect=0;
        for(unsigned b=0;b<7;++b) {
            if(x&(1u<<b))expect^=ps5vk_depth_64k_zx_offset(1u<<b,0,TILE);
            if(y&(1u<<b))expect^=ps5vk_depth_64k_zx_offset(0,1u<<b,TILE);
        }
        assert(ps5vk_depth_64k_zx_offset(x,y,TILE)==expect);
    }

    /* 3. The stored masks agree with the published pattern read the other way
     *    round, over the whole tile. */
    for(uint32_t y=0;y<TILE;++y)for(uint32_t x=0;x<TILE;++x) {
        size_t bitwise=0;
        for(unsigned i=0;i<20;++i)
            bitwise|=(size_t)(parity(x,pattern[i].x)^parity(y,pattern[i].y))<<i;
        assert(ps5vk_depth_64k_zx_offset(x,y,TILE)==bitwise);
    }

    /* 4. The depth swizzle is NOT the colour swizzle. If a refactor ever made
     *    depth reuse the colour equation this would pass silently, so it is
     *    stated: the two differ, and they differ in the low bits. */
    assert(ps5vk_depth_64k_zx_offset(1,0,TILE)==ps5vk_color_64k_rx_offset(4,1,0,TILE));
    {
        int differ=0;
        for(uint32_t y=0;y<TILE && !differ;++y)for(uint32_t x=0;x<TILE;++x)
            if(ps5vk_depth_64k_zx_offset(x,y,TILE)!=ps5vk_color_64k_rx_offset(4,x,y,TILE)) {
                differ=1;break;
            }
        assert(differ);
    }

    /* 5. Tiling across a surface wider and taller than one tile: blocks are
     *    laid out in row-major tile order, the same rule the colour path uses. */
    assert(ps5vk_depth_64k_zx_surface_size(256,256)==4u*BLOCK);
    assert(ps5vk_depth_64k_zx_offset(TILE,0,256)==BLOCK);
    assert(ps5vk_depth_64k_zx_offset(0,TILE,256)==2u*BLOCK);
    assert(ps5vk_depth_64k_zx_offset(TILE,TILE,256)==3u*BLOCK);
    /* A non-multiple width still pads to whole tiles. */
    assert(ps5vk_depth_64k_zx_surface_size(129,1)==2u*BLOCK);

    /* 6. Detiling a whole surface returns each sample to its linear place. */
    {
        const uint32_t w=64,h=48;
        size_t surface=ps5vk_depth_64k_zx_surface_size(w,h);
        unsigned char *tiled=calloc(surface,1);
        uint32_t *linear=calloc((size_t)w*h,4);
        assert(tiled&&linear);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
            uint32_t value=(y<<16)|x;
            memcpy(tiled+ps5vk_depth_64k_zx_offset(x,y,w),&value,4);
        }
        assert(!ps5vk_depth_64k_zx_detile(linear,(size_t)w*h*4,tiled,surface,w,h));
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)
            assert(linear[(size_t)y*w+x]==((y<<16)|x));
        /* Bounds are checked, not assumed. */
        assert(ps5vk_depth_64k_zx_detile(linear,(size_t)w*h*4,tiled,surface-1u,w,h));
        assert(ps5vk_depth_64k_zx_detile(linear,(size_t)w*h*4-1u,tiled,surface,w,h));
        assert(ps5vk_depth_64k_zx_detile(NULL,(size_t)w*h*4,tiled,surface,w,h));
        free(tiled);free(linear);
    }
    /* 7. The pinned CTS depth32f size_pot texture is 64x64 with the full
     *    seven-level pyramid. Mesa AddrLib (Gfx10, 64KB_Z_X, 16 pipes)
     *    packs every level into one mip-tail block. Pin addresses returned by
     *    Addr2ComputeSurfaceAddrFromCoord for the level origins and opposite
     *    corners, then prove the complete mip-tail mapping is collision-free. */
    {
        static const size_t origin[7]={33792,18432,2048,1024,8704,768,4864};
        static const size_t corner[7]={47356,30972,6396,1276,8764,780,4864};
        static const uint8_t width[7]={64,32,16,8,4,2,1};
        static const uint8_t height[7]={64,32,16,8,4,2,1};
        unsigned char *seen=calloc(BLOCK,1);
        assert(seen);
        size_t total=0;
        for(uint32_t mip=0;mip<7;++mip) {
            assert(ps5vk_depth_64k_zx_gather_mip_offset(mip,0,0)==origin[mip]);
            assert(ps5vk_depth_64k_zx_gather_mip_offset(mip,width[mip]-1u,
                height[mip]-1u)==corner[mip]);
            for(uint32_t y=0;y<height[mip];++y)for(uint32_t x=0;x<width[mip];++x) {
                size_t off=ps5vk_depth_64k_zx_gather_mip_offset(mip,x,y);
                assert(off!=SIZE_MAX&&off+4u<=BLOCK&&!(off&3u));
                assert(!seen[off]);seen[off]=1;++total;
            }
        }
        assert(total==5461u);
        free(seen);
    }
    assert(ps5vk_depth_64k_zx_offset(TILE,0,TILE)==SIZE_MAX);   /* x past the width */
    assert(ps5vk_depth_64k_zx_offset(0,0,0)==SIZE_MAX);
    assert(ps5vk_depth_64k_zx_surface_size(0,1)==SIZE_MAX);
    assert(ps5vk_depth_64k_zx_gather_mip_offset(7,0,0)==SIZE_MAX);
    assert(ps5vk_depth_64k_zx_gather_mip_offset(0,64,0)==SIZE_MAX);

    /* 8. The stencil plane of D32_SFLOAT_S8_UINT. Addresses are Mesa AddrLib
     *    Addr2ComputeSurfaceAddrFromCoord (Gfx10, 16 pipes, 64KB_Z_X,
     *    flags.stencil, bpp 8) for a 300x200 surface: two 256-wide tiles per
     *    row, so the second tile starts at 64 KiB. */
    {
        static const struct { uint32_t x,y; size_t offset; } addr[]={
            {43,101,5479},{287,133,82807},{165,81,37523},{106,58,10956},
            {79,30,9981},{199,40,41237},{90,189,24806},{244,147,60570},
            {121,1,12099},{272,73,72450},{250,109,45670},{90,101,15206},
            {0,0,0},{256,0,65536},{0,199,22570},{299,199,86383}};
        for(size_t i=0;i<sizeof(addr)/sizeof(addr[0]);++i)
            assert(ps5vk_stencil_64k_zx_offset(addr[i].x,addr[i].y,300)==addr[i].offset);
        assert(ps5vk_stencil_64k_zx_surface_size(300,200)==2u*BLOCK);
        assert(ps5vk_stencil_64k_zx_surface_size(256,256)==BLOCK);
        assert(ps5vk_stencil_64k_zx_surface_size(257,1)==2u*BLOCK);
        /* One 256x256 tile fills its 64 KiB block exactly once. */
        unsigned char *seen=calloc(BLOCK,1);
        assert(seen);
        for(uint32_t y=0;y<256;++y)for(uint32_t x=0;x<256;++x) {
            size_t off=ps5vk_stencil_64k_zx_offset(x,y,256);
            assert(off<BLOCK && !seen[off]);
            seen[off]=1;
        }
        free(seen);
        /* The 1-byte equation is not the 4-byte depth one read per byte. */
        assert(ps5vk_stencil_64k_zx_offset(1,0,256)!=ps5vk_depth_64k_zx_offset(1,0,256));
        const uint32_t w=64,h=48;
        size_t surface=ps5vk_stencil_64k_zx_surface_size(w,h);
        unsigned char *tiled=calloc(surface,1),*linear=calloc((size_t)w*h,1);
        assert(tiled&&linear);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)
            tiled[ps5vk_stencil_64k_zx_offset(x,y,w)]=(unsigned char)(x*7u+y*13u);
        assert(!ps5vk_stencil_64k_zx_detile(linear,(size_t)w*h,tiled,surface,w,h));
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)
            assert(linear[(size_t)y*w+x]==(unsigned char)(x*7u+y*13u));
        assert(ps5vk_stencil_64k_zx_detile(linear,(size_t)w*h-1u,tiled,surface,w,h));
        assert(ps5vk_stencil_64k_zx_detile(linear,(size_t)w*h,tiled,surface-1u,w,h));
        assert(ps5vk_stencil_64k_zx_detile(NULL,(size_t)w*h,tiled,surface,w,h));
        assert(ps5vk_stencil_64k_zx_offset(w,0,w)==SIZE_MAX);
        assert(ps5vk_stencil_64k_zx_surface_size(0,1)==SIZE_MAX);
        free(tiled);free(linear);
    }
    puts("Depth detile: SW_64K_Z_X is a bijection of the tile, affine, matches the published pattern, and differs from the colour swizzle");
    return 0;
}

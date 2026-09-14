/*
 * Copyright (C) 2026 Manuel Pereira
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Descending padded-linear mip packing is adapted from ps5-opengl,
 * src/gallium/ps5/ps5_screen.c at
 * 7f9bfabdddb187a11e4401058eba8c9e55194d0a.
 */
#include "texture_layout.h"
#include "texture_format.h"

/* Checked products. Every quantity is computed before anything is written, so a
 * rejected geometry leaves the caller's structure untouched. */
static int checked_product(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a && b > UINT64_MAX / a) return -1;
    *out = a * b;
    return 0;
}

int ps5vk_texture_mip_layout_for_slices(VkFormat format,uint32_t width,
    uint32_t height,uint32_t storage_layers,uint32_t level_count,
    struct ps5vk_texture_mip_layout *out)
{
    struct ps5vk_texture_mip_layout result={0};
    /* The padded-linear encoding exists for every implemented sampled-image
     * capability; the published role is a separate, witnessed decision. */
    if(!ps5vk_texture_format_sampled_encoding(format))
        return -1;
    const struct ps5vk_texture_format *entry=ps5vk_texture_format_lookup(format);
    if(!out || !entry || !entry->bytes_per_texel || !width || !height ||
       !storage_layers || !level_count ||
       level_count>PS5VK_MAX_TEXTURE_MIP_LEVELS || width>16384 || height>16384)
        return -1;
    result.level_count=level_count;result.storage_layers=storage_layers;
    result.alignment=256;
    for(uint32_t level=level_count;level--;) {
        uint64_t divisor=UINT64_C(1)<<level;
        uint64_t storage_width=((uint64_t)width+divisor-1)/divisor;
        uint64_t storage_height=((uint64_t)height+divisor-1)/divisor;
        if(!storage_width)storage_width=1;
        if(!storage_height)storage_height=1;
        uint64_t row_bytes;
        if(storage_height>UINT32_MAX ||
           checked_product(storage_width,entry->bytes_per_texel,&row_bytes) ||
           row_bytes>UINT32_MAX)return -1;
        uint64_t pitch=(row_bytes+255u)&~UINT64_C(255);
        uint64_t level_bytes;
        if(pitch>UINT32_MAX || checked_product(pitch,storage_height,&level_bytes) ||
           level_bytes>UINT64_MAX-result.layer_stride)return -1;
        result.levels[level]=(struct ps5vk_texture_mip_level){
            result.layer_stride,(uint32_t)pitch,(uint32_t)storage_width,
            (uint32_t)storage_height};
        result.layer_stride+=level_bytes;
    }
    uint64_t total;
    if(checked_product(result.layer_stride,storage_layers,&total))return -1;
    result.bytes=total;
    *out=result;return 0;
}
int ps5vk_texture_layout_for_slices(VkFormat format,uint32_t width,uint32_t height,
    uint32_t slices,struct ps5vk_texture_layout *out)
{
    struct ps5vk_texture_mip_layout chain;
    if(!out || ps5vk_texture_mip_layout_for_slices(format,width,height,slices,1,&chain))
        return -1;
    *out=(struct ps5vk_texture_layout){chain.levels[0].row_pitch,chain.bytes,
        chain.alignment,chain.layer_stride};return 0;
}
int ps5vk_texture_layout_for_format(VkFormat format,uint32_t width,uint32_t height,
    struct ps5vk_texture_layout *out)
{
    return ps5vk_texture_layout_for_slices(format,width,height,1,out);
}
int ps5vk_texture_layout(uint32_t width,uint32_t height,struct ps5vk_texture_layout *out)
{
    return ps5vk_texture_layout_for_format(VK_FORMAT_R8G8B8A8_UNORM,width,height,out);
}

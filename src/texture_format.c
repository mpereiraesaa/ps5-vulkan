/*
 * Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFX10.3 image-format words and component selectors adapted from
 * BlackBearReloaded's ps5-opengl, src/gallium/ps5/ps5_screen.c at commit
 * 7f9bfabdddb187a11e4401058eba8c9e55194d0a (GPL-3.0-or-later).
 */
#include "texture_format.h"

/* Vulkan identity swizzles: missing colour components read as zero and a
 * missing alpha component reads as one. GFX10 selectors are X/Y/Z/W=4/5/6/7,
 * constant zero/one=0/1. All rows have exact PS5 creation, upload, sampling
 * and readback evidence. Filterable rows additionally have two byte-identical
 * nearest/linear checkerboard runs; integer rows have typed nearest-only runs,
 * as required by Vulkan. */
static const struct ps5vk_texture_format formats[] = {
    {VK_FORMAT_R8_UNORM,          1, UINT32_C(0x00100000), {4,0,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R8_SNORM,          1, UINT32_C(0x00200000), {4,0,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R8G8_UNORM,        2, UINT32_C(0x00e00000), {4,5,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R8G8_SNORM,        2, UINT32_C(0x00f00000), {4,5,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_UNORM,    4, UINT32_C(0x03800000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_SNORM,    4, UINT32_C(0x03900000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_SRGB,     4, UINT32_C(0x08200000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, UINT32_C(0x08400000), {4,5,6,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32,4, UINT32_C(0x02400000), {4,5,6,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16_UNORM,         2, UINT32_C(0x00700000), {4,0,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16_SNORM,         2, UINT32_C(0x00800000), {4,0,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16_SFLOAT,        2, UINT32_C(0x00d00000), {4,0,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16G16_UNORM,      4, UINT32_C(0x01700000), {4,5,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16G16_SNORM,      4, UINT32_C(0x01800000), {4,5,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16G16_SFLOAT,     4, UINT32_C(0x01d00000), {4,5,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16G16B16A16_UNORM,8,UINT32_C(0x04100000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16G16B16A16_SNORM,8,UINT32_C(0x04200000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R16G16B16A16_SFLOAT, 8, UINT32_C(0x04700000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R32_SFLOAT,        4, UINT32_C(0x01600000), {4,0,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R32G32_SFLOAT,     8, UINT32_C(0x04000000), {4,5,0,1}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R32G32B32A32_SFLOAT,16, UINT32_C(0x04d00000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    /* Integer rows use typed isampler/usampler interfaces. Vulkan forbids
     * linear filtering for integer formats, so these remain nearest-only. */
    {VK_FORMAT_R8_UINT,           1, UINT32_C(0x00500000), {4,0,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8_SINT,           1, UINT32_C(0x00600000), {4,0,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8G8_UINT,         2, UINT32_C(0x01200000), {4,5,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8G8_SINT,         2, UINT32_C(0x01300000), {4,5,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_UINT,     4, UINT32_C(0x03c00000), {4,5,6,7}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_SINT,     4, UINT32_C(0x03d00000), {4,5,6,7}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R16_UINT,          2, UINT32_C(0x00b00000), {4,0,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R16_SINT,          2, UINT32_C(0x00c00000), {4,0,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R16G16_UINT,       4, UINT32_C(0x01b00000), {4,5,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R16G16_SINT,       4, UINT32_C(0x01c00000), {4,5,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R16G16B16A16_UINT,8, UINT32_C(0x04500000), {4,5,6,7}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R16G16B16A16_SINT,8, UINT32_C(0x04600000), {4,5,6,7}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R32_UINT,          4, UINT32_C(0x01400000), {4,0,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R32_SINT,          4, UINT32_C(0x01500000), {4,0,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R32G32_UINT,       8, UINT32_C(0x03e00000), {4,5,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R32G32_SINT,       8, UINT32_C(0x03f00000), {4,5,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R32G32B32A32_UINT,16,UINT32_C(0x04b00000), {4,5,6,7}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R32G32B32A32_SINT,16,UINT32_C(0x04c00000), {4,5,6,7}, VK_FALSE, VK_TRUE},
};

const struct ps5vk_texture_format *ps5vk_texture_format_lookup(VkFormat format)
{
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
        if (formats[i].format == format) return &formats[i];
    return 0;
}

VkBool32 ps5vk_texture_format_supported(VkFormat format)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    return entry && entry->validated;
}

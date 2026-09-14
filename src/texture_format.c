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
 * and readback evidence. Linear filtering is separately proven only for
 * RGBA8_UNORM. */
static const struct ps5vk_texture_format formats[] = {
    {VK_FORMAT_R8_UNORM,          1, UINT32_C(0x00100000), {4,0,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8_SNORM,          1, UINT32_C(0x00200000), {4,0,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8G8_UNORM,        2, UINT32_C(0x00e00000), {4,5,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8G8_SNORM,        2, UINT32_C(0x00f00000), {4,5,0,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_UNORM,    4, UINT32_C(0x03800000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_SNORM,    4, UINT32_C(0x03900000), {4,5,6,7}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_SRGB,     4, UINT32_C(0x08200000), {4,5,6,7}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, UINT32_C(0x08400000), {4,5,6,1}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R16G16B16A16_SFLOAT, 8, UINT32_C(0x04700000), {4,5,6,7}, VK_FALSE, VK_TRUE},
    {VK_FORMAT_R32G32B32A32_SFLOAT,16, UINT32_C(0x04d00000), {4,5,6,7}, VK_FALSE, VK_TRUE},
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

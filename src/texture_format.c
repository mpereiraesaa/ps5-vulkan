/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFX10.3 image-format words and component selectors adapted from
 * BlackBearReloaded's ps5-opengl, src/gallium/ps5/ps5_screen.c at commit
 * 7f9bfabdddb187a11e4401058eba8c9e55194d0a (GPL-3.0-or-later).
 */
#include "texture_format.h"

#ifndef PS5VK_ENABLE_TEXTURE_FORMAT_CANDIDATES
#define PS5VK_ENABLE_TEXTURE_FORMAT_CANDIDATES 0
#endif

/* Vulkan identity swizzles: missing colour components read as zero and a
 * missing alpha component reads as one. GFX10 selectors are X/Y/Z/W=4/5/6/7,
 * constant zero/one=0/1. Only RGBA8_UNORM has PS5 hardware evidence in this
 * repository today; the other rows remain diagnostic candidates. */
static const struct ps5vk_texture_format formats[] = {
    {VK_FORMAT_R8_UNORM,          1, UINT32_C(0x00100000), {4,0,0,1}, VK_TRUE, VK_FALSE},
    {VK_FORMAT_R8G8_UNORM,        2, UINT32_C(0x00e00000), {4,5,0,1}, VK_TRUE, VK_FALSE},
    {VK_FORMAT_R8G8B8A8_UNORM,    4, UINT32_C(0x03800000), {4,5,6,7}, VK_TRUE, VK_TRUE},
    {VK_FORMAT_R8G8B8A8_SRGB,     4, UINT32_C(0x08200000), {4,5,6,7}, VK_TRUE, VK_FALSE},
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
    return entry && (entry->validated || PS5VK_ENABLE_TEXTURE_FORMAT_CANDIDATES);
}

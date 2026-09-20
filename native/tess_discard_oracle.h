// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PS5VK_TESS_DISCARD_ORACLE_H
#define PS5VK_TESS_DISCARD_ORACLE_H
/* Pinned Vulkan-Docs f84d432d, chapters/tessellation.adoc, Patch Discard:
 * relevant outer levels<=0 discard; NaN discards on NaN-supporting hardware.
 * Irrelevant outer levels and nonpositive inner levels must not discard.
 * This oracle expects the NaN-supporting case and must not be weakened merely
 * because a native measurement differs. It measures visibility, not counters. */
static inline int ps5vk_tess_patch_survives(unsigned domain,unsigned patch)
{
    if(domain>2 || patch>16)return 0;
    const unsigned outer=domain==0?3u:domain==1?4u:2u;
    return patch==0 || patch>=13 || (patch-1)/3>=outer;
}
static inline int ps5vk_tess_discard_pixel(unsigned domain,unsigned x,unsigned y)
{
    if(x<6 || y<6 || (x-6)%12 || (y-6)%12)return -1;
    unsigned column=(x-6)/12,row=(y-6)/12;
    if(column>=5 || row>=4)return -1;
    unsigned patch=row*5+column;
    return ps5vk_tess_patch_survives(domain,patch)?(int)patch:-1;
}
#endif

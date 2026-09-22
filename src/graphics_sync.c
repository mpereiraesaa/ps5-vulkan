/* Copyright (C) 2026 Manuel Pereira
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The colour-to-texture release packet is adapted from ps5-opengl,
 * src/platform/ps5_agc_runtime_backend.c at commit
 * 7f9bfabdddb187a11e4401058eba8c9e55194d0a (GPL-3.0-or-later). */
#include "graphics_sync.h"
#include <string.h>
const uint32_t ps5vk_graphics_probe_registers[PS5VK_GRAPHICS_PROBE_REGISTERS]={
    0xa090,0xa091,0xa094,0xa095,0xa292,0xa10f,0xa110,0xa111,0xa112,0xa200,
    0xa204,0xa206,0xa311,0xa293
};
size_t ps5vk_graphics_occlusion_event(uint32_t *out,size_t capacity,uint64_t address)
{
    /* PKT3_EVENT_WRITE with a two-dword payload: the event/type word followed
     * by the 48-bit address. PKT3 encodes bits 31:30 as 0b11, the payload
     * count minus one at 29:16 and the opcode at 15:8, so EVENT_WRITE (0x46)
     * with three payload dwords is 0xC0024600. EVENT_TYPE occupies bits 5:0
     * (ZPASS_DONE is 21) and EVENT_INDEX bits 11:8 (1 selects the dump). */
    if(!out || capacity<4 || !address || (address&7) ||
       address>(UINT64_C(1)<<48)-8)return 0;
    const uint32_t words[4]={UINT32_C(0xC0024600),UINT32_C(0x0115),
        (uint32_t)address,(uint32_t)(address>>32)};
    memcpy(out,words,sizeof(words));return 4;
}
size_t ps5vk_graphics_register_probe(uint32_t *out,size_t capacity,uint64_t destination)
{
    const size_t bytes=PS5VK_GRAPHICS_PROBE_REGISTERS*4;
    if(!out || capacity<PS5VK_GRAPHICS_PROBE_WORDS || !destination || destination%4 ||
       destination>(UINT64_C(1)<<48)-bytes)return 0;
    uintptr_t start=(uintptr_t)out;
    if(start>UINTPTR_MAX-PS5VK_GRAPHICS_PROBE_WORDS*4 ||
       (destination<start+PS5VK_GRAPHICS_PROBE_WORDS*4 && destination+bytes>start))return 0;
    uint32_t words[PS5VK_GRAPHICS_PROBE_WORDS];
    for(unsigned i=0;i<PS5VK_GRAPHICS_PROBE_REGISTERS;++i) {
        uint64_t address=destination+i*4;
        /* Same bootstrap compute profile COPY_DATA route: register source, TC_L2 destination,
         * confirmed 32-bit write. Addresses here are DWORD register indices. */
        uint32_t p[6]={0xc0044000,0x00100200,ps5vk_graphics_probe_registers[i],0,
            (uint32_t)address,(uint32_t)(address>>32)};
        memcpy(words+i*6,p,sizeof(p));
    }
    memcpy(out,words,sizeof(words));return PS5VK_GRAPHICS_PROBE_WORDS;
}
size_t ps5vk_graphics_acquire(uint32_t *out, size_t capacity)
{
    if (!out || capacity < PS5VK_GRAPHICS_ACQUIRE_WORDS) return 0;
    /* RADV radv_emit_cp_dma: CP_SYNC waits for DMA in ME; graphics additionally
     * synchronizes PFP before fetching dependent data. Cache invalidation alone
     * does not express that ordering. Retain the full-range cache operation. */
    const uint32_t words[10] = {0xc0004200,0,0xc0065800,0,0xffffffff,0xff,0,0,10,0x4381};
    memcpy(out,words,sizeof(words)); return PS5VK_GRAPHICS_ACQUIRE_WORDS;
}
size_t ps5vk_graphics_color_to_texture(uint32_t *out, size_t capacity)
{
    if (!out || capacity < PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WORDS) return 0;
    /* BlackBearReloaded's GPL ps5-opengl runtime backend uses this exact
     * GFX10 RELEASE_MEM packet for render-to-texture: event 0x2d flushes CB,
     * then the GCR control performs GLM/GLV/GL1/GL2 writeback/invalidation.
     * A generic acquire does not flush dirty colour-backend data and therefore
     * left a following subpassLoad observing the earlier clear value. */
    const uint32_t words[PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WORDS] = {
        UINT32_C(0xc0064900), UINT32_C(0x0070f52d),
        UINT32_C(0x00010000), 0, 0, 0, 0, 0,
    };
    memcpy(out, words, sizeof(words));
    return PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WORDS;
}
size_t ps5vk_graphics_color_to_texture_wait(uint32_t *out, size_t capacity,
    uint64_t address, uint32_t token)
{
    uint32_t words[PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WAIT_WORDS];
    if (!out || capacity < PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WAIT_WORDS || !token || !address ||
        (address & 7) || address > (UINT64_C(1)<<48)-8) return 0;
    uintptr_t start=(uintptr_t)out;
    if (start > UINTPTR_MAX-32 || (address < start+32 && address+8 > start)) return 0;
    /* PKT3_RELEASE_MEM with six payload words: the CB data-flush event and the
     * reference's GCR action bits, then DST_SEL=MEM(0) |
     * INT_SEL=SEND_DATA_AFTER_WR_CONFIRM(3) << 24 | DATA_SEL=VALUE_32BIT(1) <<
     * 29, the address the confirmation writes, and the token itself. The
     * trailing word is the packet's unused one, kept exactly as the eight-word
     * reference packet emits it. */
    const uint32_t release[8] = {UINT32_C(0xc0064900), UINT32_C(0x0070f52d),
        UINT32_C(0x23000000), (uint32_t)address, (uint32_t)(address>>32), token, 0, 0};
    /* PKT3_WAIT_REG_MEM: equality, memory space, ME engine, like the release
     * path's own wait. */
    const uint32_t wait[7] = {UINT32_C(0xc0053c00), UINT32_C(0x13),
        (uint32_t)address, (uint32_t)(address>>32), token, UINT32_C(0xffffffff), 4};
    memcpy(words, release, sizeof(release));
    memcpy(words+8, wait, sizeof(wait));
    memcpy(out, words, sizeof(words));
    return PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WAIT_WORDS;
}
size_t ps5vk_graphics_release(uint32_t *out, size_t capacity, uint64_t address, uint64_t serial)
{
    if (!out || capacity < PS5VK_GRAPHICS_RELEASE_WORDS || !serial || !address ||
        (address & 7) || address > (UINT64_C(1)<<48)-8) return 0;
    uintptr_t start=(uintptr_t)out;
    /* Prevent a completion write from overwriting this release packet. The
     * batch builder must also reject overlap with the rest of its command span. */
    if (start > UINTPTR_MAX-32 || (address < start+32 && address+8 > start)) return 0;
    /* Public Mesa gfx10: event 0x14 CACHE_FLUSH_AND_INV_TS_EVENT, index 5;
     * CB/DB precede GLM WB/INV, GLV/GL1 INV, GL2 WB/INV via forward sequence.
     * 0x28 is BOTTOM_OF_PIPE_TS, not a CB/DB cache-flush event. A completion
     * token alone does not make render-backend cache contents CPU-visible.
     * Unlike Gears' flip-tail, this does not rely on a preceding SetFlip. */
    const uint32_t words[8] = {0xc0064900,0x0070f514,0x42010000,
        (uint32_t)address,(uint32_t)(address>>32),(uint32_t)serial,(uint32_t)(serial>>32),0};
    memcpy(out,words,sizeof(words)); return 8;
}

size_t ps5vk_graphics_release_wait(uint32_t *out,size_t capacity,uint64_t address,uint32_t token)
{
    uint32_t words[15];
    if(!out || capacity<15 || !token)return 0;
    uintptr_t start=(uintptr_t)out;
    if(start>UINTPTR_MAX-sizeof(words) ||
       (address<start+sizeof(words) && address+8>start))return 0;
    if(!ps5vk_graphics_release(words,8,address,token))return 0;
    /* Public gfx10 WAIT_REG_MEM: equality, memory space, ME engine.
     * RELEASE_MEM alone is asynchronous: CP DMA could otherwise race earlier
     * CB writes and a later cache writeback would overwrite the new clear. */
    const uint32_t wait[7]={0xc0053c00,0x13,(uint32_t)address,
        (uint32_t)(address>>32),token,0xffffffff,4};
    memcpy(words+8,wait,sizeof(wait));memcpy(out,words,sizeof(words));return 15;
}

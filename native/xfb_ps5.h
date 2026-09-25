#ifndef PS5VK_XFB_PS5_H
#define PS5VK_XFB_PS5_H
#include <stdint.h>
#include <string.h>

/* Transform feedback execution (DXVK262-T14).
 *
 * A capture program compiled with the pinned compiler's no-GDS streamout
 * (psbc ps5_global_streamout) reads one user-data dword: the low half of a
 * table of five buffer records whose high half is address32_hi. Records 0..3
 * are the bound capture ranges; record 4 is the begin's control block, on
 * which each workgroup reserves its range with global atomics
 * (ac_nir_prerast_utils.c, the use_ps5_global_streamout branches):
 *
 *   u32 buffer_offsets[4]        byte offset written so far, per buffer
 *   u32 generated_primitives[4]  per stream
 *   u32 emitted_primitives[4]    per stream
 *
 * A transform feedback session (begin..end) owns one control block and one
 * table. The queue loads buffer_offsets from the counter buffers at begin (or
 * leaves them zero) and copies them back at end, which is exactly the Vulkan
 * counter: the byte offset, relative to the bound offset, where capture
 * continues. Slot 0 of every job is the idle session a capture pipeline draws
 * through while capture is not active: four null records (nothing is
 * written) and a scratch control block. */
enum {
    PS5VK_XFB_SESSION_BYTES = 256,
    PS5VK_XFB_CONTROL_OFFSET = 0,
    PS5VK_XFB_CONTROL_BYTES = 64,
    PS5VK_XFB_TABLE_OFFSET = 64,
    PS5VK_XFB_TABLE_RECORDS = 5,
    PS5VK_XFB_GENERATED_OFFSET = 16,
    PS5VK_XFB_EMITTED_OFFSET = 32,
    PS5VK_XFB_DMA_WORDS = 7,
};
/* The raw-buffer record format the lab's streamout reference and the
 * tessellation rings use (32-bit float X, untyped, stride 0). */
#define PS5VK_XFB_RECORD_FORMAT UINT32_C(0x31016fac)

/* One raw buffer record: base, no stride, a byte-sized range. A zero range is
 * the null record the compiled program treats as "not bound". Returns 0 when
 * the base is outside the 48-bit GPU range or unaligned. */
static inline int ps5vk_xfb_record(uint32_t out[4], uint64_t address, uint32_t bytes)
{
    if(!bytes) { memset(out, 0, 16); return 1; }
    if(!address || (address & 3u) || address >= (UINT64_C(1) << 48) ||
       (bytes & 3u))return 0;
    out[0] = (uint32_t)address;
    out[1] = (uint32_t)(address >> 32) & 0xffffu;
    out[2] = bytes;
    out[3] = PS5VK_XFB_RECORD_FORMAT;
    return 1;
}

/* A session's table: the four capture records and the control record. The
 * table and control addresses are those of the session itself. */
static inline int ps5vk_xfb_table(uint32_t table[PS5VK_XFB_TABLE_RECORDS * 4],
    const uint64_t address[4], const uint32_t bytes[4], uint64_t control)
{
    for(unsigned b = 0; b < 4; ++b)
        if(!ps5vk_xfb_record(table + 4u * b, bytes[b] ? address[b] : 0u, bytes[b]))
            return 0;
    return ps5vk_xfb_record(table + 16, control, PS5VK_XFB_CONTROL_BYTES);
}

/* PKT3 DMA_DATA of one dword, L2 to L2, with CP_SYNC so the micro engine does
 * not run ahead of the copy (the lab-validated route of src/texture_dma.c). */
static inline int ps5vk_xfb_copy_dword(uint32_t out[PS5VK_XFB_DMA_WORDS],
                                       uint64_t source, uint64_t destination)
{
    if(!source || !destination || (source & 3u) || (destination & 3u) ||
       source >= (UINT64_C(1) << 48) || destination >= (UINT64_C(1) << 48))return 0;
    out[0] = UINT32_C(0xc0055000);
    out[1] = UINT32_C(0xe0300000);
    out[2] = (uint32_t)source; out[3] = (uint32_t)(source >> 32);
    out[4] = (uint32_t)destination; out[5] = (uint32_t)(destination >> 32);
    out[6] = 4u;
    return 1;
}
#endif

#ifndef PS5VK_GRAPHICS_SYNC_H
#define PS5VK_GRAPHICS_SYNC_H
#include <stddef.h>
#include <stdint.h>
enum { PS5VK_GRAPHICS_ACQUIRE_WORDS = 10, PS5VK_GRAPHICS_RELEASE_WORDS = 8,
    PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WORDS = 8 };
enum { PS5VK_GRAPHICS_PROBE_REGISTERS=14, PS5VK_GRAPHICS_PROBE_WORDS=84 };
extern const uint32_t ps5vk_graphics_probe_registers[PS5VK_GRAPHICS_PROBE_REGISTERS];
/* Diagnostic COPY_DATA reads into separate owned GPU-visible storage. */
size_t ps5vk_graphics_register_probe(uint32_t *,size_t,uint64_t);
/* GFX10 occlusion-counter event write. Emits PKT3_EVENT_WRITE with
 * EVENT_TYPE(V_028A90_ZPASS_DONE) and EVENT_INDEX(1) towards the given slot
 * address: the begin call addresses the slot base and the end call base+8, and
 * every enabled render backend then writes its 64-bit start/end pair with
 * availability bit 63 set. Returns 0 for a null, misaligned or out-of-range
 * address, or too little capacity, and writes nothing in that case. */
size_t ps5vk_graphics_occlusion_event(uint32_t *, size_t, uint64_t);
/* Full-range GFX10 cache operations for the first serial graphics backend.
 * Caller supplies mapped owned storage and flushes CPU writes before submit.
 * RELEASE signals an exact nonzero 64-bit serial after CB/DB and GCR work.
 * These are NOT a VideoOut acquisition/presentation contract. */
size_t ps5vk_graphics_acquire(uint32_t *, size_t capacity);
/* Flush one colour attachment from the render backend and make it visible to
 * texture reads in the following subpass. This is the bounded CB -> texture
 * transition, not a completion signal and not a replacement for acquire. */
size_t ps5vk_graphics_color_to_texture(uint32_t *, size_t capacity);
size_t ps5vk_graphics_release(uint32_t *, size_t capacity, uint64_t address, uint64_t serial);
#endif

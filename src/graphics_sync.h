#ifndef PS5VK_GRAPHICS_SYNC_H
#define PS5VK_GRAPHICS_SYNC_H
#include <stddef.h>
#include <stdint.h>
enum { PS5VK_GRAPHICS_ACQUIRE_WORDS = 10, PS5VK_GRAPHICS_RELEASE_WORDS = 8,
    PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WORDS = 8,
    PS5VK_GRAPHICS_COLOR_TO_TEXTURE_WAIT_WORDS = 15 };
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
/* The same barrier, made SYNCHRONOUS.
 *
 * A RELEASE_MEM is asynchronous: the packet retires when the event is accepted
 * and the colour-block writeback it starts continues behind it, so a following
 * draw that READS the attachment through the texture path can see whatever the
 * caches have not written back yet. Measured on the pinned multisample leaves
 * as a resolved image in which only some 8x8 tiles carried the drawn value,
 * different tiles on every run.
 *
 * This form carries the completion token the pinned RADV emitter uses for the
 * same event (mesh-gfx10 `gfx10_cs_emit_cache_flush`): the RELEASE_MEM names a
 * 32-bit value to be stored when the writeback is CONFIRMED (DST_SEL=MEM,
 * INT_SEL=SEND_DATA_AFTER_WR_CONFIRM, DATA_SEL=VALUE_32BIT) and a WAIT_REG_MEM
 * for it follows, so everything recorded afterwards reads memory the engine has
 * already made visible. The address is a private word inside the command arena
 * that executes the wait, never the label poll() watches. */
size_t ps5vk_graphics_color_to_texture_wait(uint32_t *, size_t, uint64_t address, uint32_t token);
size_t ps5vk_graphics_release(uint32_t *, size_t capacity, uint64_t address, uint64_t serial);
/* Drain prior render work before a following ME/CP DMA write. The scratch
 * token is distinct from the submission completion label and must start zero. */
size_t ps5vk_graphics_release_wait(uint32_t *,size_t,uint64_t,uint32_t);
#endif

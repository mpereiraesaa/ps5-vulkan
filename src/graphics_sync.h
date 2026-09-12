#ifndef PS5VK_GRAPHICS_SYNC_H
#define PS5VK_GRAPHICS_SYNC_H
#include <stddef.h>
#include <stdint.h>
enum { PS5VK_GRAPHICS_ACQUIRE_WORDS = 10, PS5VK_GRAPHICS_RELEASE_WORDS = 8 };
enum { PS5VK_GRAPHICS_PROBE_REGISTERS=14, PS5VK_GRAPHICS_PROBE_WORDS=84 };
extern const uint32_t ps5vk_graphics_probe_registers[PS5VK_GRAPHICS_PROBE_REGISTERS];
/* Diagnostic COPY_DATA reads into separate owned GPU-visible storage. */
size_t ps5vk_graphics_register_probe(uint32_t *,size_t,uint64_t);
/* Full-range GFX10 cache operations for the first serial graphics backend.
 * Caller supplies mapped owned storage and flushes CPU writes before submit.
 * RELEASE signals an exact nonzero 64-bit serial after CB/DB and GCR work.
 * These are NOT a VideoOut acquisition/presentation contract. */
size_t ps5vk_graphics_acquire(uint32_t *, size_t capacity);
size_t ps5vk_graphics_release(uint32_t *, size_t capacity, uint64_t address, uint64_t serial);
#endif

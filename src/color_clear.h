#ifndef PS5VK_COLOR_CLEAR_H
#define PS5VK_COLOR_CLEAR_H
#include <stdint.h>
/* Scoped finite [0,1] RGBA float clear -> little-endian BGRA8 UNORM.
 * Reject unsupported values without modifying the output. */
int ps5vk_color_clear_bgra8(const float rgba[4], uint32_t *out);
int ps5vk_color_clear_rgba8(const float rgba[4], uint32_t *out);
/* An integer colour target's clear is not converted: the four unsigned
 * components are the 8-bit lanes of one 32-bit word, little-endian, exactly as
 * the surface stores them (R in byte zero). A component that does not fit the
 * format is refused rather than truncated. */
int ps5vk_color_clear_rgba8_uint(const uint32_t rgba[4], uint32_t *out);
#endif

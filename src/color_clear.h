#ifndef PS5VK_COLOR_CLEAR_H
#define PS5VK_COLOR_CLEAR_H
#include <stdint.h>
/* Scoped finite [0,1] RGBA float clear -> little-endian BGRA8 UNORM.
 * Reject unsupported values without modifying the output. */
int ps5vk_color_clear_bgra8(const float rgba[4], uint32_t *out);
int ps5vk_color_clear_rgba8(const float rgba[4], uint32_t *out);
/* Integer clear values are packed into R,G,B,A byte lanes. UINT keeps each
 * component's low eight bits; out-of-range SINT values are refused because
 * Vulkan leaves their conversion undefined. */
int ps5vk_color_clear_rgba8_uint(const uint32_t rgba[4], uint32_t *out);
int ps5vk_color_clear_rgba8_sint(const int32_t rgba[4], uint32_t *out);
#endif

#ifndef PS5VK_DEPTH_LAYOUT_H
#define PS5VK_DEPTH_LAYOUT_H
#include <stdint.h>
struct ps5vk_depth_layout {
    uint32_t width, height, pitch, padded_height;
    uint64_t bytes, alignment;
    uint32_t swizzle_mode;
};
/* GFX10 64KB_Z_X, 2D D32, one mip/layer/sample, no HTILE. This header is the
 * footprint only, which does not require the pipe XOR topology. The pixel
 * addressing that does is in src/depth_detile.c. */
int ps5vk_depth_layout(uint32_t width, uint32_t height, struct ps5vk_depth_layout *out);
/* The diagnostic D16 attachment is deliberately one 128x128, one-sample
 * 64KB_Z_X surface. Its 16-bit texels occupy one 64 KiB block. No general
 * D16 tiling or transfer-addressing role is implied. */
int ps5vk_depth16_layout(uint32_t width, uint32_t height, struct ps5vk_depth_layout *out);
#endif

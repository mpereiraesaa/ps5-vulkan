#ifndef PS5VK_DEPTH_LAYOUT_H
#define PS5VK_DEPTH_LAYOUT_H
#include <stdint.h>
struct ps5vk_depth_layout {
    uint32_t width, height, pitch, padded_height;
    uint64_t bytes, alignment;
    uint32_t swizzle_mode;
};
/* GFX10 64KB_Z_X, 2D D32, one mip/layer/sample, no HTILE. Footprint does
 * not require pipe XOR topology; pixel addressing DOES and is not supplied. */
int ps5vk_depth_layout(uint32_t width, uint32_t height, struct ps5vk_depth_layout *out);
#endif

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
/* D32_SFLOAT_S8_UINT: GFX10 stores the two aspects as separate planes. The
 * depth plane is exactly the D32 64KB_Z_X surface above (AddrLib computes it
 * with the stencil flag set and returns the same equation and footprint).
 * The stencil plane is an 8-bit 64KB_Z_X surface - one 64 KiB block holds a
 * 256x256 tile - placed at the depth plane's size rounded up to the stencil
 * plane's 64 KiB base alignment, which is Mesa ac_surface's gfx9+ rule
 * (stencil_offset = align(surf_size, baseAlign)). One mip, one layer, one
 * sample, no HTILE. */
struct ps5vk_depth_stencil_layout {
    struct ps5vk_depth_layout depth;
    uint32_t stencil_pitch, stencil_padded_height;
    uint64_t stencil_offset, stencil_bytes, bytes, alignment;
};
int ps5vk_depth_stencil_layout(uint32_t width, uint32_t height,
    struct ps5vk_depth_stencil_layout *out);
#endif

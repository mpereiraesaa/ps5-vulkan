#ifndef PS5VK_TEXTURE_LAYOUT_H
#define PS5VK_TEXTURE_LAYOUT_H
#include <stdint.h>
struct ps5vk_texture_layout { uint32_t row_pitch; uint64_t bytes,alignment; };
/* Internal one-level RGBA8 sampling layout, not a Vulkan linear-tiling promise.
 * Xash3D/GFX10 sampling profile: 256-byte base and row alignment. */
int ps5vk_texture_layout(uint32_t,uint32_t,struct ps5vk_texture_layout *);
#endif

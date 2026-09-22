#ifndef PS5VK_DEPTH_DETILE_H
#define PS5VK_DEPTH_DETILE_H
#include <stddef.h>
#include <stdint.h>
/* GFX10 SW_64K_Z_X pixel addressing for a single-sample D32 surface: the
 * depth counterpart of src/color_detile.h. One 64 KiB block holds a 128x128
 * tile of 4-byte samples. */
size_t ps5vk_depth_64k_zx_surface_size(uint32_t width,uint32_t height);
size_t ps5vk_depth_64k_zx_offset(uint32_t x,uint32_t y,uint32_t width);
int ps5vk_depth_64k_zx_detile(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t width,uint32_t height);
#endif

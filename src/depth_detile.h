#ifndef PS5VK_DEPTH_DETILE_H
#define PS5VK_DEPTH_DETILE_H
#include <stddef.h>
#include <stdint.h>
/* GFX10 SW_64K_Z_X pixel addressing for a single-sample D32 surface: the
 * depth counterpart of src/color_detile.h. One 64 KiB block holds a 128x128
 * tile of 4-byte samples. */
size_t ps5vk_depth_64k_zx_surface_size(uint32_t width,uint32_t height);
size_t ps5vk_depth_64k_zx_offset(uint32_t x,uint32_t y,uint32_t width);
/* Exact 64x64 D32, seven-level GFX10 mip tail used by the pinned T07 gather
 * leaf. Coordinates and offsets follow Mesa AddrLib's ADDR_SW_64KB_Z_X
 * mip-tail calculation, with the already cross-validated 16-pipe equation. */
size_t ps5vk_depth_64k_zx_gather_mip_offset(uint32_t mip,uint32_t x,uint32_t y);
int ps5vk_depth_64k_zx_detile(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t width,uint32_t height);
/* The stencil plane of D32_SFLOAT_S8_UINT: GFX10 SW_64K_Z_X for a 1-byte
 * element, one 64 KiB block per 256x256 tile, blocks in row-major tile
 * order. Derived from the same AddrLib table and 16-pipe configuration as the
 * depth equation above (see src/depth_detile.c). */
size_t ps5vk_stencil_64k_zx_surface_size(uint32_t width,uint32_t height);
size_t ps5vk_stencil_64k_zx_offset(uint32_t x,uint32_t y,uint32_t width);
/* Writes width*height bytes, one stencil value per texel, tightly packed:
 * the VK_FORMAT_S8_UINT buffer layout of a stencil-aspect copy. */
int ps5vk_stencil_64k_zx_detile(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t width,uint32_t height);
#endif

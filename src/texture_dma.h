#ifndef PS5VK_TEXTURE_DMA_H
#define PS5VK_TEXTURE_DMA_H
#include "texture_copy.h"
#include <stddef.h>
/* GFX10 graphics CP DMA, L2-to-L2, one packet per row. CP_SYNC on the
 * final row orders the copy engine; caller still owes shader cache invalidation
 * and a completion fence. No CPU data copy, allocation, or submission. */
size_t ps5vk_texture_dma(uint32_t *,size_t,uint64_t,uint64_t,
    const struct ps5vk_texture_copy *);
/* Uniform 32-bit GPU fill; same completion/cache obligations as the copy. */
size_t ps5vk_dma_fill(uint32_t *,size_t,uint64_t,uint64_t,uint32_t);
#endif

#ifndef PS5VK_COLOR_DETILE_H
#define PS5VK_COLOR_DETILE_H
#include <stddef.h>
#include <stdint.h>
size_t ps5vk_rgba8_64k_rx_offset(uint32_t x,uint32_t y,uint32_t width);
int ps5vk_rgba8_64k_rx_detile(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t width,uint32_t height);
#endif

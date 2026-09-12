#ifndef PS5VK_GRAPHICS_PIPELINE_PS5_H
#define PS5VK_GRAPHICS_PIPELINE_PS5_H
#include "vk_internal.h"
#include "graphics_pair.h"
struct ps5vk_native_graphics_pipeline {
    VkDevice device;
    struct ps5vk_memory_backend memory;
    void *backing;
    struct ps5vk_graphics_pair *pair;
    /* Empty table for the current no-resource procedural shader ABI. Same
     * high 32 address bits as both stages, retained with their allocation. */
    const uint32_t *global_table;
    size_t allocation_bytes;
};
VkResult ps5vk_native_graphics_create(VkDevice, const void *, void **);
void ps5vk_native_graphics_release(VkDevice, void *);
#endif

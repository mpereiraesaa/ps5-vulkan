#ifndef PS5VK_VERTEX_FETCH_H
#define PS5VK_VERTEX_FETCH_H
#include "vk_command.h"
#include "graphics_program.h"
struct ps5vk_vertex_fetch {
    const void *address;
    VkDeviceSize bytes;
    uint32_t stride;
    uint32_t attribute_extent;
};
/* Resolve the byte-granular Vulkan binding without assuming that the native
 * SRD can encode its low address bits.  Native preparation may stage an
 * unaligned span into job-owned storage before constructing the descriptor.
 * Output is zeroed for a zero-count draw and unchanged on failure. */
VkResult ps5vk_vertex_fetch_span(VkDevice,const struct ps5vk_graphics_key *,
    const struct ps5vk_operation *,struct ps5vk_vertex_fetch *);
/* Direct descriptor helper for already SRD-aligned spans. Output is unchanged
 * on failure and on zero-count draws. */
VkResult ps5vk_vertex_fetch_descriptor(VkDevice,const struct ps5vk_graphics_key *,
    const struct ps5vk_operation *,uint32_t out[4]);
#endif

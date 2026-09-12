#ifndef PS5VK_VERTEX_FETCH_H
#define PS5VK_VERTEX_FETCH_H
#include "vk_command.h"
#include "graphics_program.h"
/* Initial compiled profile: one per-vertex binding zero, interleaved float
 * attributes. Resolves both memory binding and command binding offsets.
 * Does not upload or retain resources; pending command ownership does that.
 * Output is unchanged on failure and on zero-count draws. */
VkResult ps5vk_vertex_fetch_descriptor(VkDevice,const struct ps5vk_graphics_key *,
    const struct ps5vk_operation *,uint32_t out[4]);
#endif

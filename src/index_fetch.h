#ifndef PS5VK_INDEX_FETCH_H
#define PS5VK_INDEX_FETCH_H
#include "vk_command.h"
struct ps5vk_index_fetch {
    uint64_t address;
    uint32_t available_count;
    uint32_t element_bytes;
};
/* Resolve bound memory + binding offset + firstIndex without reading or
 * rewriting index contents. Output unchanged on failure or an empty draw.
 * Caller retains the recorded buffer through GPU completion. */
VkResult ps5vk_index_fetch_prepare(VkDevice,const struct ps5vk_operation *,
    struct ps5vk_index_fetch *);
#endif

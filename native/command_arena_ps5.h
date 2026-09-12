#ifndef PS5VK_COMMAND_ARENA_PS5_H
#define PS5VK_COMMAND_ARENA_PS5_H
#include "vk_internal.h"
enum { PS5VK_COMMAND_ARENA_BYTES = 131072, PS5VK_COMMAND_ARENA_ALIGNMENT = 65536,
       PS5VK_COMMAND_ARENA_WORDS = (PS5VK_COMMAND_ARENA_BYTES - 64) / 4 };
struct ps5vk_command_arena {
    void *address;
    int64_t physical;
    unsigned reserved, allocated, mapped, uncertain;
};
/* Initially zero. A DEVICE_LOST result retains the arena: do not erase the
 * record or release any related GPU resources after ambiguous mapping changes.
 * Release is legal only before submit or after exact GPU retirement. */
VkResult ps5vk_command_arena_create(struct ps5vk_command_arena *);
VkResult ps5vk_command_arena_release(struct ps5vk_command_arena *);
volatile uint64_t *ps5vk_command_arena_label(struct ps5vk_command_arena *);
#endif

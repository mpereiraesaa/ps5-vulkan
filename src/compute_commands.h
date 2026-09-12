#ifndef PS5VK_COMPUTE_COMMANDS_H
#define PS5VK_COMPUTE_COMMANDS_H
#include <stddef.h>
#include <stdint.h>
#define PS5VK_COMPLETION_VALUE UINT64_C(0x13579bdf2468ace0)

/* Specific to the pinned bootstrap compute profile LLPC shader; not a general Vulkan pipeline ABI. */
struct ps5vk_compute_addresses {
    uint64_t code, descriptor_table, completion, readback;
};
enum { PS5VK_COMPUTE_COMMAND_CAPACITY = 96 };
/* All addresses must already be mapped/owned. This only validates encoding
 * constraints, never proves mapping, visibility, execution or completion. */
size_t ps5vk_compute_commands(uint32_t *words, size_t capacity,
                             const struct ps5vk_compute_addresses *addresses);
#endif

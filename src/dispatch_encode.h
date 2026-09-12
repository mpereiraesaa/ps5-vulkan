#ifndef PS5VK_DISPATCH_ENCODE_H
#define PS5VK_DISPATCH_ENCODE_H
#include "compute_commands.h"
#include "vk_pipeline.h"
struct ps5vk_dispatch_encoding {
    const struct ps5vk_compiled_program *program;
    struct ps5vk_compute_addresses addresses;
    uint32_t groups[3];
    uint64_t completion_value;
};
/* Encodes an owned/mapped GPU dispatch; mapping, submission and waiting belong
 * to the native queue. Rejection never modifies the destination array. */
size_t ps5vk_dispatch_encode(uint32_t *words, size_t capacity,
                            const struct ps5vk_dispatch_encoding *dispatch);
#endif

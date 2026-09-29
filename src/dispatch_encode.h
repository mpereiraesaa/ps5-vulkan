#ifndef PS5VK_DISPATCH_ENCODE_H
#define PS5VK_DISPATCH_ENCODE_H
#include "compute_commands.h"
#include "vk_pipeline.h"
/* PS5 compute profile: 36 CUs, 32 wave32 slots per CU. */
enum { PS5VK_COMPUTE_SCRATCH_WAVES = 1152, PS5VK_SCRATCH_GUARD_BYTES = 16384 };
/* Native placement of a compute program: a 64-byte prefix that selects
 * instruction prefetch mode 3 and branches to the program, the program, then
 * s_code_end padding beyond the prefetch distance. PS5 waves start without
 * the forward instruction prefetch that Linux configures for GFX10
 * (SH_MEM_CONFIG.INITIAL_INST_PREFETCH = 3), so straight-line programs larger
 * than the instruction cache otherwise wait on every cache line. */
enum { PS5VK_CODE_PREFIX_BYTES = 64, PS5VK_CODE_TAIL_BYTES = 256 };
static inline uint64_t ps5vk_placed_code_bytes(size_t code_words)
{ return PS5VK_CODE_PREFIX_BYTES + (uint64_t)code_words * 4 + PS5VK_CODE_TAIL_BYTES; }
/* Writes ps5vk_placed_code_bytes(code_words) bytes at destination. */
void ps5vk_place_code(void *destination, const uint32_t *code, size_t code_words);
struct ps5vk_dispatch_encoding {
    const struct ps5vk_compiled_program *program;
    struct ps5vk_compute_addresses addresses;
    uint64_t descriptor_tables[PS5VK_MAX_SETS];
    uint64_t push_constants;
    uint64_t scratch, scratch_bytes;
    uint32_t groups[3];
    uint32_t group_base[3];
    uint64_t completion_value;
};
/* Encodes an owned/mapped GPU dispatch; mapping, submission and waiting belong
 * to the native queue. Rejection never modifies the destination array. */
size_t ps5vk_dispatch_encode(uint32_t *words, size_t capacity,
                            const struct ps5vk_dispatch_encoding *dispatch);
#endif

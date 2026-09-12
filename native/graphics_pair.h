#ifndef PS5VK_GRAPHICS_PAIR_H
#define PS5VK_GRAPHICS_PAIR_H
#include "ps5_shader_header.h"
#include "ps5_agc_registers.h"
#include "shader_relocate.h"

struct ps5vk_graphics_pair {
    struct ps5_shader_arena gs, ps;
    struct ps5_agc_linked_cx cx;
    struct ps5_agc_linked_uc uc;
    unsigned ready;
    uint32_t vertex_quantization;
};
struct ps5vk_graphics_stage_extent { uint32_t offset, isa_bytes; };
struct ps5vk_graphics_pair_input {
    const void *image;
    size_t image_bytes;
    const struct ps5vk_shader_relocation *relocations;
    size_t relocation_count;
    struct ps5vk_graphics_stage_extent gs, ps;
    const struct ps5_shader_metadata *metadata;
    uint32_t vertex_quantization;
    const ps5_agc_register *interpolators;
    uint32_t interpolator_count;
};
/* Caller owns disjoint writable pair/image storage for the full pipeline life.
 * Pair must initially be zero-initialized, and source image must be disjoint.
 * Native memory must be CPU/GPU identity-mapped. No submission or allocation is
 * performed here. Caller publishes/flushes both after success, before GPU use.
 * A failed preparation is not usable and has no GPU ownership to retire. */
int ps5vk_graphics_pair_prepare(struct ps5vk_graphics_pair *pair,
    void *mapped_image, size_t capacity, const struct ps5vk_graphics_pair_input *input);
#endif

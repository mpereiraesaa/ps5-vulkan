#ifndef PS5VK_SHADER_RELOCATE_H
#define PS5VK_SHADER_RELOCATE_H
#include <stddef.h>
#include <stdint.h>
struct ps5vk_shader_relocation {
    uint32_t offset, symbol_offset, addend, type;
};
/* Applies audited AMDGPU ABS32_LO/HI REL records to an owned image containing
 * code and constants. Returns zero on success; failure changes no bytes.
 * This is address relocation, not upload, cache management or execution. */
int ps5vk_shader_relocate(void *image, size_t bytes, uint64_t gpu_base,
    const struct ps5vk_shader_relocation *relocations, size_t count);
#endif

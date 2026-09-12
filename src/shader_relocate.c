#include "shader_relocate.h"

int ps5vk_shader_relocate(void *image, size_t bytes, uint64_t base,
    const struct ps5vk_shader_relocation *relocations, size_t count)
{
    if (!image || !bytes || bytes > 16 * 1024 * 1024 || !base || (base & 255) ||
        base >= (UINT64_C(1) << 48) || bytes > (UINT64_C(1) << 48) - base ||
        (count && !relocations) || count > bytes / 4) return -1;
    for (size_t i = 0; i < count; ++i) {
        const struct ps5vk_shader_relocation *r = &relocations[i];
        uint64_t relative = (uint64_t)r->symbol_offset + r->addend;
        if ((r->type != 1 && r->type != 2) || r->offset % 4 ||
            r->offset > bytes || bytes - r->offset < 4 || relative >= bytes)
            return -1;
        for (size_t j = 0; j < i; ++j)
            if (relocations[j].offset == r->offset) return -1;
    }
    unsigned char *data = image;
    for (size_t i = 0; i < count; ++i) {
        const struct ps5vk_shader_relocation *r = &relocations[i];
        /* LLVM AMDGPU ABI: implicit 32-bit REL addends are zero-extended,
         * including ABS32_HI. Compute S+A before selecting the high half. */
        uint64_t value = base + r->symbol_offset + (uint64_t)r->addend;
        uint32_t part = (uint32_t)(r->type == 1 ? value : value >> 32);
        for (unsigned b = 0; b < 4; ++b) data[r->offset + b] = (unsigned char)(part >> (b * 8));
    }
    return 0;
}

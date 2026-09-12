#include "dispatch_encode.h"
#include <string.h>

static int overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return a < b + bn && b < a + an; }

size_t ps5vk_dispatch_encode(uint32_t *words, size_t capacity,
                            const struct ps5vk_dispatch_encoding *d)
{
    if (!words || !d || !d->program || capacity < PS5VK_COMPUTE_COMMAND_CAPACITY ||
        !d->completion_value) return 0;
    const struct ps5vk_compiled_program *p = d->program;
    const struct ps5vk_compute_addresses *a = &d->addresses;
    if (p->gfx != 1013 || p->wave_size != 32 || !p->code_words || p->code_words > 1024 * 1024 ||
        !p->vgprs || p->vgprs > 256 || !p->sgprs || p->sgprs > 106 || p->user_sgprs != 2 ||
        p->float_mode > 255 || p->ieee_mode > 1 || p->mem_ordered > 1 || p->tg_size > 1 ||
        p->tidig_components > 2 || !p->descriptor_count || p->descriptor_count > PS5VK_MAX_BINDINGS)
        return 0;
    uint64_t local = 1, table_bytes = 0;
    for (unsigned j = 0; j < 3; ++j) {
        if (!p->local_size[j] || p->local_size[j] > 1024 || d->groups[j] > 65535 || p->tgid[j] > 1)
            return 0;
        local *= p->local_size[j];
    }
    if (local > 1024) return 0;
    for (uint32_t j = 0; j < p->descriptor_count; ++j) {
        const struct ps5vk_program_descriptor *b = &p->descriptors[j];
        if (b->set || b->element || b->binding >= PS5VK_MAX_BINDINGS || b->table_dword >= 128 || b->table_dword % 4)
            return 0;
        uint64_t end = (b->table_dword + 4) * 4;
        if (end > table_bytes) table_bytes = end;
        for (uint32_t k = 0; k < j; ++k)
            if (p->descriptors[k].table_dword == b->table_dword || p->descriptors[k].binding == b->binding)
                return 0;
    }
    uint64_t code_bytes = p->code_words * 4;
    if (a->code > (UINT64_C(1) << 48) - code_bytes ||
        a->descriptor_table > (UINT64_C(1) << 48) - table_bytes ||
        a->completion > (UINT64_C(1) << 48) - 8 || a->readback > (UINT64_C(1) << 48) - 16 ||
        (a->code >> 32) != ((a->code + code_bytes - 1) >> 32) ||
        (a->descriptor_table >> 32) != ((a->descriptor_table + table_bytes - 1) >> 32)) return 0;
    uint64_t bases[] = {a->code, a->descriptor_table, a->completion, a->readback};
    uint64_t sizes[] = {code_bytes, table_bytes, 8, 16};
    for (unsigned j = 0; j < 4; ++j)
        for (unsigned k = 0; k < j; ++k)
            if (overlap(bases[j], sizes[j], bases[k], sizes[k])) return 0;

    /* Reuse bootstrap compute profile's proven preamble/cache/completion sequence unchanged, then
     * substitute every shader/dispatch-dependent field by decoded packet tags.
     * No shader ISA is embedded in the template. bootstrap compute profile remains independently
     * buildable and is the byte-for-byte reference for its original parameters. */
    uint32_t packet[PS5VK_COMPUTE_COMMAND_CAPACITY];
    size_t n = ps5vk_compute_commands(packet, PS5VK_COMPUTE_COMMAND_CAPACITY, a);
    if (!n) return 0;
    /* gfx10.json COMPUTE_PGM_RSRC1/2 and RADV's wave32 allocation granule.
     * SGPRS field is unused for this GFX10 ABI; scratch/LDS remain disabled. */
    uint32_t rsrc1 = ((p->vgprs - 1) / 8) | (p->float_mode << 12) |
        (p->ieee_mode << 23) | (p->mem_ordered << 30);
    uint32_t rsrc2 = (p->user_sgprs << 1) | (p->tgid[0] << 7) | (p->tgid[1] << 8) |
        (p->tgid[2] << 9) | (p->tg_size << 10) | (p->tidig_components << 11);
    unsigned found = 0;
    for (size_t i = 0; i < n;) {
        if ((packet[i] >> 30) != 3) return 0;
        size_t total = ((packet[i] >> 16) & 0x3fff) + 2;
        if (total > n - i) return 0;
        uint32_t opcode = (packet[i] >> 8) & 0xff;
        if (opcode == 0x76 && total >= 3) {
            uint32_t reg = 0xb000 + packet[i + 1] * 4;
            if (reg == 0xb81c) {
                if (total != 5 || (found & 1)) return 0;
                memcpy(packet + i + 2, p->local_size, 12); found |= 1;
            } else if (reg == 0xb848) {
                if (total != 4 || (found & 2)) return 0;
                packet[i + 2] = rsrc1; packet[i + 3] = rsrc2; found |= 2;
            }
        } else if (opcode == 0x15) {
            if (total != 5 || (found & 4)) return 0;
            memcpy(packet + i + 1, d->groups, 12); found |= 4;
        } else if (opcode == 0x49) {
            if (total != 8 || (found & 8) || packet[i + 1] != 0x0070f528) return 0;
            packet[i + 5] = (uint32_t)d->completion_value;
            packet[i + 6] = (uint32_t)(d->completion_value >> 32); found |= 8;
        }
        i += total;
    }
    if (found != 15) return 0;
    memcpy(words, packet, n * sizeof(*words));
    return n;
}

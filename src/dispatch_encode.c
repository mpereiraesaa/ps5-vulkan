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
        !p->vgprs || p->vgprs > 256 || !p->sgprs || p->sgprs > 106 ||
        p->user_sgprs < 2 || p->user_sgprs > 10 || p->lds_size > 128 ||
        p->float_mode > 255 || p->ieee_mode > 1 || p->mem_ordered > 1 || p->tg_size > 1 ||
        p->tidig_components > 2 || p->descriptor_count > PS5VK_MAX_DESCRIPTORS ||
        (p->descriptor_set_mask & ~((1u << PS5VK_MAX_SETS) - 1)) ||
        (!!p->descriptor_count != !!p->descriptor_set_mask) ||
        p->push_constant_size > PS5VK_MAX_PUSH_CONSTANT_BYTES)
        return 0;
    uint64_t local = 1, table_bytes[PS5VK_MAX_SETS] = {0};
    for (unsigned j = 0; j < 3; ++j) {
        if (!p->local_size[j] || p->local_size[j] > 1024 || d->groups[j] > 65535 || p->tgid[j] > 1)
            return 0;
        local *= p->local_size[j];
    }
    if (local > 1024) return 0;
    for (uint32_t j = 0; j < p->descriptor_count; ++j) {
        const struct ps5vk_program_descriptor *b = &p->descriptors[j];
        if (b->set >= PS5VK_MAX_SETS || !(p->descriptor_set_mask & (1u << b->set)) ||
            b->binding >= PS5VK_MAX_BINDINGS || b->table_dword >= 128 || b->table_dword % 4)
            return 0;
        uint64_t end = (b->table_dword + 4) * 4;
        if (end > table_bytes[b->set]) table_bytes[b->set] = end;
        for (uint32_t k = 0; k < j; ++k)
            if (p->descriptors[k].set == b->set &&
                (p->descriptors[k].table_dword == b->table_dword ||
                 (p->descriptors[k].binding == b->binding && p->descriptors[k].element == b->element)))
                return 0;
    }
    VkBool32 legacy = p->user_sgprs == 2 && p->descriptor_set_mask == 1 &&
        p->descriptor_set_sgpr[0] == 1 && !p->push_constant_size &&
        !p->push_constant_sgpr && !p->grid_size_sgpr;
    if (legacy) {
        if (!d->descriptor_tables[0] || d->descriptor_tables[0] != a->descriptor_table ||
            d->push_constants) return 0;
        for (uint32_t set = 1; set < PS5VK_MAX_SETS; ++set)
            if (p->descriptor_set_sgpr[set] || d->descriptor_tables[set]) return 0;
    } else {
        uint32_t next_sgpr = 2;
        for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set) {
            if (!(p->descriptor_set_mask & (1u << set))) {
                if (p->descriptor_set_sgpr[set] || d->descriptor_tables[set]) return 0;
                continue;
            }
            if (p->descriptor_set_sgpr[set] != next_sgpr++ || !d->descriptor_tables[set] ||
                (d->descriptor_tables[set] & 15u) || (d->descriptor_tables[set] >> 32) != 2 ||
                d->descriptor_tables[set] > (UINT64_C(1) << 48) - table_bytes[set] ||
                (d->descriptor_tables[set] >> 32) !=
                    ((d->descriptor_tables[set] + table_bytes[set] - 1) >> 32)) return 0;
        }
        if (p->push_constant_size) {
            if (p->push_constant_sgpr != next_sgpr++ || !d->push_constants ||
                (d->push_constants & 3u) || (d->push_constants >> 32) != 2 ||
                d->push_constants > (UINT64_C(1) << 48) - p->push_constant_size ||
                (d->push_constants >> 32) !=
                    ((d->push_constants + p->push_constant_size - 1) >> 32)) return 0;
        } else if (p->push_constant_sgpr || d->push_constants) return 0;
        if (p->grid_size_sgpr) {
            if (p->grid_size_sgpr != next_sgpr) return 0;
            next_sgpr += 3;
        }
        if (p->user_sgprs != next_sgpr) return 0;
    }
    uint64_t code_bytes = p->code_words * 4;
    if (a->code > (UINT64_C(1) << 48) - code_bytes ||
        a->completion > (UINT64_C(1) << 48) - 8 || a->readback > (UINT64_C(1) << 48) - 16 ||
        (a->code >> 32) != ((a->code + code_bytes - 1) >> 32)) return 0;
    uint64_t bases[4 + PS5VK_MAX_SETS] = {a->code, a->completion, a->readback};
    uint64_t sizes[4 + PS5VK_MAX_SETS] = {code_bytes, 8, 16};
    unsigned regions = 3;
    for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set)
        if (p->descriptor_set_mask & (1u << set)) {
            bases[regions] = d->descriptor_tables[set]; sizes[regions++] = table_bytes[set];
        }
    if (p->push_constant_size) {
        bases[regions] = d->push_constants; sizes[regions++] = p->push_constant_size;
    }
    for (unsigned j = 0; j < regions; ++j)
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
     * SGPRS field is unused for this GFX10 ABI; scratch remains disabled. */
    uint32_t rsrc1 = ((p->vgprs - 1) / 8) | (p->float_mode << 12) |
        (p->ieee_mode << 23) | (p->wgp_mode << 29) | (p->mem_ordered << 30);
    uint32_t rsrc2 = (p->user_sgprs << 1) | (p->tgid[0] << 7) | (p->tgid[1] << 8) |
        (p->tgid[2] << 9) | (p->tg_size << 10) | (p->tidig_components << 11) |
        (p->lds_size << 15);
    unsigned found = 0;
    size_t out = 0;
    for (size_t i = 0; i < n;) {
        if ((packet[i] >> 30) != 3) return 0;
        size_t total = ((packet[i] >> 16) & 0x3fff) + 2;
        if (total > n - i) return 0;
        uint32_t opcode = (packet[i] >> 8) & 0xff;
        if (opcode == 0x76 && total >= 3) {
            uint32_t reg = 0xb000 + packet[i + 1] * 4;
            if (reg == 0xb81c) {
                if (total != 5 || (found & 1) || out + 5 > capacity) return 0;
                memcpy(words + out, packet + i, 2 * sizeof(uint32_t));
                memcpy(words + out + 2, p->local_size, 12);
                out += 5; found |= 1;
            } else if (reg == 0xb848) {
                if (total != 4 || (found & 2) || out + 4 > capacity) return 0;
                words[out] = packet[i]; words[out + 1] = packet[i + 1];
                words[out + 2] = rsrc1; words[out + 3] = rsrc2;
                out += 4; found |= 2;
            } else if (reg == 0xb900 && !legacy) {
                size_t count = p->user_sgprs + 2;
                if (total != 4 || (found & 16) || out + count > capacity) return 0;
                words[out] = UINT32_C(0xc0007600) | (p->user_sgprs << 16);
                words[out + 1] = packet[i + 1];
                words[out + 2] = 0;
                words[out + 3] = 0;
                memset(words + out + 2, 0, p->user_sgprs * sizeof(uint32_t));
                for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set)
                    if (p->descriptor_set_mask & (1u << set))
                        words[out + 2 + p->descriptor_set_sgpr[set]] = (uint32_t)d->descriptor_tables[set];
                if (p->push_constant_size)
                    words[out + 2 + p->push_constant_sgpr] = (uint32_t)d->push_constants;
                if (p->grid_size_sgpr)
                    memcpy(words + out + 2 + p->grid_size_sgpr, d->groups, 3 * sizeof(uint32_t));
                out += count; found |= 16;
            } else {
                if (out + total > capacity) return 0;
                memcpy(words + out, packet + i, total * sizeof(uint32_t));
                out += total;
            }
        } else if (opcode == 0x15) {
            if (total != 5 || (found & 4) || out + 5 > capacity) return 0;
            words[out] = packet[i];
            memcpy(words + out + 1, d->groups, 12);
            words[out + 4] = packet[i + 4];
            out += 5; found |= 4;
        } else if (opcode == 0x49) {
            if (total != 8 || (found & 8) || packet[i + 1] != 0x0070f528 || out + 8 > capacity) return 0;
            memcpy(words + out, packet + i, 5 * sizeof(uint32_t));
            words[out + 5] = (uint32_t)d->completion_value;
            words[out + 6] = (uint32_t)(d->completion_value >> 32);
            words[out + 7] = packet[i + 7];
            out += 8; found |= 8;
        } else {
            if (out + total > capacity) return 0;
            memcpy(words + out, packet + i, total * sizeof(uint32_t));
            out += total;
        }
        i += total;
    }
    unsigned expected_mask = legacy ? 15 : 31;
    if (found != expected_mask) return 0;
    return out;
}

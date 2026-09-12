#include "descriptor_encode.h"
#include <string.h>

VkResult ps5vk_descriptor_encode(VkDevice device,
    const struct ps5vk_compiled_program *program, VkDescriptorSet set,
    uint32_t *table, size_t capacity_dwords)
{
    if (!device || !program || !set || !set->pool || set->pool->device != device ||
        !table || program->gfx != 1013 || !program->descriptor_count ||
        program->descriptor_count > PS5VK_MAX_BINDINGS)
        return VK_ERROR_UNKNOWN;
    uint32_t scratch[128] = {0};
    size_t extent = 0;
    for (uint32_t i = 0; i < program->descriptor_count; ++i) {
        const struct ps5vk_program_descriptor *p = &program->descriptors[i];
        if (p->set || p->element || p->binding >= PS5VK_MAX_BINDINGS ||
            p->table_dword > 124 || p->table_dword % 4 ||
            capacity_dwords < p->table_dword + 4)
            return VK_ERROR_UNKNOWN;
        for (uint32_t j = 0; j < i; ++j)
            if (program->descriptors[j].table_dword == p->table_dword ||
                program->descriptors[j].binding == p->binding)
                return VK_ERROR_UNKNOWN;
        const struct ps5vk_binding *binding = &set->signature.binding[p->binding];
        /* A scalar compiler resource may use element zero of a larger layout
         * binding. Do not reject a layout already accepted by pipeline creation;
         * actual compiler array elements remain outside this ABI. */
        if (set->signature.combined_image[p->binding] || !binding->count || binding->first >= PS5VK_MAX_DESCRIPTORS ||
            !set->defined[binding->first]) return VK_ERROR_UNKNOWN;
        const VkDescriptorBufferInfo *info = &set->buffers[binding->first];
        void *address = NULL; VkDeviceSize bytes = 0;
        VkResult result = ps5vk_buffer_span(device, info->buffer, info->offset,
                                           info->range, &address, &bytes);
        if (result != VK_SUCCESS) return result;
        uint64_t gpu = (uintptr_t)address;
        if (!bytes || bytes > UINT32_MAX || gpu >= (UINT64_C(1) << 48) ||
            bytes > (UINT64_C(1) << 48) - gpu) return VK_ERROR_UNKNOWN;
        uint32_t *out = scratch + p->table_dword;
        out[0] = (uint32_t)gpu; out[1] = (uint32_t)(gpu >> 32);
        out[2] = (uint32_t)bytes;
        /* bootstrap compute profile's validated byte-addressed raw descriptor: stride zero, byte
         * extent. Do not replace NUM_RECORDS with a count of uint32 elements. */
        out[3] = 0x31016fac;
        if (extent < p->table_dword + 4) extent = p->table_dword + 4;
    }
    memcpy(table, scratch, extent * sizeof(*table));
    return VK_SUCCESS;
}

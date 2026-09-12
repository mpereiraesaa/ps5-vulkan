#include "descriptor_encode.h"
#include <string.h>

VkResult ps5vk_descriptor_encode(VkDevice device,
    const struct ps5vk_compiled_program *program, uint32_t set_index, VkDescriptorSet set,
    uint32_t *table, size_t capacity_dwords)
{
    if (!device || !program || !set || !set->pool || set->pool->device != device ||
        !table || program->gfx != 1013 || !program->descriptor_count ||
        set_index >= PS5VK_MAX_SETS || program->descriptor_count > PS5VK_MAX_DESCRIPTORS)
        return VK_ERROR_UNKNOWN;
    uint32_t scratch[128] = {0};
    size_t extent = 0;
    for (uint32_t i = 0; i < program->descriptor_count; ++i) {
        const struct ps5vk_program_descriptor *p = &program->descriptors[i];
        if (p->set != set_index) continue;
        if (p->binding >= PS5VK_MAX_BINDINGS ||
            p->table_dword > 124 || p->table_dword % 4 ||
            capacity_dwords < p->table_dword + 4)
            return VK_ERROR_UNKNOWN;
        for (uint32_t j = 0; j < i; ++j)
            if (program->descriptors[j].set == set_index &&
                (program->descriptors[j].table_dword == p->table_dword ||
                 (program->descriptors[j].binding == p->binding &&
                  program->descriptors[j].element == p->element)))
                return VK_ERROR_UNKNOWN;
        const struct ps5vk_binding *binding = &set->signature.binding[p->binding];
        uint32_t index = binding->first + p->element;
        if (binding->count <= p->element || index >= PS5VK_MAX_DESCRIPTORS ||
            !set->defined[index] || set->signature.type[p->binding] != p->type)
            return VK_ERROR_UNKNOWN;
        if (p->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            VkBufferView view = set->texel_views[index];
            if (!view || view->device != device || !view->buffer) return VK_ERROR_UNKNOWN;
            void *address = NULL; VkDeviceSize bytes = 0;
            VkResult result = ps5vk_buffer_span(device, view->buffer, view->offset,
                                                view->range, &address, &bytes);
            if (result != VK_SUCCESS || !bytes || bytes / 4 > UINT32_MAX) return VK_ERROR_UNKNOWN;
            uint64_t gpu = (uintptr_t)address;
            uint32_t format = view->format == VK_FORMAT_R32_UINT ? 20u :
                view->format == VK_FORMAT_R32_SINT ? 21u : 22u;
            uint32_t *out = scratch + p->table_dword;
            /* Match RADV's gfx10 texel-buffer descriptor contract: a 4-byte
             * structured element, NUM_RECORDS in texels, structured OOB
             * selection, and RESOURCE_LEVEL set.  A raw/stride-zero descriptor
             * is valid for SSBO byte addressing but makes typed texel loads
             * return the OOB value on this path. */
            out[0] = (uint32_t)gpu;
            out[1] = (uint32_t)(gpu >> 32) | (4u << 16);
            out[2] = (uint32_t)(bytes / 4);
            out[3] = UINT32_C(0x11000fac) | (format << 12);
            if (extent < p->table_dword + 4) extent = p->table_dword + 4;
            continue;
        }
        const VkDescriptorBufferInfo *info = &set->buffers[index];
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
    if(!extent)return VK_ERROR_UNKNOWN;
    memcpy(table, scratch, extent * sizeof(*table));
    return VK_SUCCESS;
}

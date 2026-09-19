#include "graphics_pipeline_ps5.h"
#include "graphics_program.h"
#include "tess_shared_storage.h"
#include <stdlib.h>
#include <string.h>

VkResult ps5vk_native_graphics_create(VkDevice d, const void *data,
    uint32_t primitive_type, void **out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = NULL;
    const struct ps5vk_graphics_pair_input *input = data;
    if (!d || !input || !input->image_bytes || input->image_bytes > 16u*1024u*1024u ||
        !d->memory.allocate || !d->memory.release || !d->memory.flush)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* The offline program library is the audited triangle-list profile and
     * ps5vk_graphics_resolve compares topology, so a strip pipeline cannot reach
     * a record here; refuse rather than link a mismatched primitive. */
    if (primitive_type != PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_native_graphics_pipeline *p = calloc(1, sizeof(*p));
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->device = d; p->memory = d->memory;
    size_t code_offset = (sizeof(struct ps5vk_graphics_pair) + 255u) & ~(size_t)255u;
    size_t table_offset = (code_offset + input->image_bytes + 15u) & ~(size_t)15u;
    p->allocation_bytes = table_offset + 16;
    void *address = NULL;
    VkResult rc = p->memory.allocate(p->memory.context, p->allocation_bytes, &address, &p->backing);
    if (rc != VK_SUCCESS) { free(p); return rc; }
    if (!address || !p->backing || ((uintptr_t)address & 255u) ||
        (uintptr_t)address >= (UINT64_C(1)<<48) ||
        p->allocation_bytes > (UINT64_C(1)<<48) - (uintptr_t)address ||
        ((uintptr_t)address >> 32) != (((uintptr_t)address + p->allocation_bytes - 1) >> 32)) {
        if (p->backing) p->memory.release(p->memory.context, p->backing);
        free(p); return VK_ERROR_MEMORY_MAP_FAILED;
    }
    p->pair = address;
    p->global_table = (const uint32_t *)((unsigned char *)address + table_offset);
    memset((unsigned char *)address + table_offset, 0, 16);
    memset(address, 0, code_offset);
    if (ps5vk_graphics_pair_prepare(p->pair, (unsigned char *)address + code_offset,
                                   input->image_bytes, input)) {
        p->memory.release(p->memory.context, p->backing); free(p);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* Publish AGC-mutated headers, relocated code/constants and owned empty
     * global table. Future descriptor ABIs must populate their own table shape.
     * The draw
     * submission must still invalidate GPU caches and retain this allocation
     * until its completion; CPU flushing alone is not a GPU dependency. */
    rc = p->memory.flush(p->memory.context, p->backing, 0, p->allocation_bytes);
    if (rc != VK_SUCCESS) {
        p->memory.release(p->memory.context, p->backing); free(p); return rc;
    }
    *out = p; return VK_SUCCESS;
}

void ps5vk_native_graphics_release(VkDevice d, void *state)
{
    struct ps5vk_native_graphics_pipeline *p = state;
    if (!p) return;
    if (!d || p->device != d) { if (d) ++d->lifetime_errors; return; }
    /* Vulkan pipeline destruction has already enforced the pending-use guard. */
    if (p->shared_rings) ps5vk_tess_storage_release(&p->shared_rings);
    else if (p->rings_backing) p->memory.release(p->memory.context, p->rings_backing);
    p->memory.release(p->memory.context, p->backing);
    free(p);
}

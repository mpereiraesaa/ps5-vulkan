#ifndef PS5VK_DESCRIPTOR_ENCODE_H
#define PS5VK_DESCRIPTOR_ENCODE_H
#include "vk_pipeline.h"
#include "descriptor_table_layout.h"
#include <string.h>
/* nullDescriptor (VK_EXT_robustness2): the record written for a
 * VK_NULL_HANDLE buffer, buffer view or image view is all zero, the GFX10
 * null-descriptor convention Mesa's RADV/ACO rely on (ac_gpu_info.c: only
 * GFX6-7 need a mapped address instead of zeroes). NUM_RECORDS zero puts
 * every buffer access out of range - loads return zero and stores are
 * discarded - and a zero T# has no image type. */
static inline void ps5vk_null_descriptor_words(uint32_t *out, uint32_t dwords)
{ memset(out, 0, (size_t)dwords * sizeof(*out)); }
/* One GFX1013 buffer record shared by the compute and graphics delivery paths.
 * `dynamic_offset` is added to the descriptor's own offset for a dynamic
 * binding and is zero otherwise. A null buffer is encoded as a null record
 * only on a device that enabled nullDescriptor. With robustBufferAccess2
 * enabled NUM_RECORDS is the range rounded up to four bytes. Failure leaves
 * the caller's words untouched. */
VkResult ps5vk_buffer_descriptor(VkDevice device,
    const VkDescriptorBufferInfo *info, VkDeviceSize dynamic_offset, uint32_t out[4]);
/* One GFX1013 typed texel-buffer V# for a view whose format row carries the
 * implemented `capability` (PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER or
 * PS5VK_FORMAT_CAP_STORAGE_TEXEL_BUFFER). Failure leaves out untouched. */
VkResult ps5vk_texel_buffer_descriptor(VkDevice device, VkBufferView view,
    uint32_t capability, uint32_t out[4]);
/* GFX1013 raw storage-buffer table for the audited compiler ABI. No uploads or
 * GPU work. Failure leaves the caller's table untouched. */
VkResult ps5vk_descriptor_encode(VkDevice device,
    const struct ps5vk_compiled_program *program, uint32_t set_index, VkDescriptorSet set,
    const VkDeviceSize dynamic_offsets[PS5VK_MAX_DYNAMIC_DESCRIPTORS],
    uint32_t *table, size_t capacity_dwords);
/* Table dwords the compiled program addresses in `set_index` (its records'
 * furthest extent), zero when it reads nothing there. */
static inline size_t ps5vk_compute_table_dwords(const struct ps5vk_compiled_program *program,
                                               uint32_t set_index)
{
    size_t dwords = 0;
    for (uint32_t i = 0; program && i < program->descriptor_count; ++i) {
        const struct ps5vk_program_descriptor *p = &program->descriptors[i];
        const size_t end = (size_t)p->table_dword + ps5vk_compute_record_dwords(p->type);
        if (p->set == set_index && end > dwords) dwords = end;
    }
    return (dwords + 3u) & ~(size_t)3u;
}
#endif

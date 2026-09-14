#ifndef PS5VK_DESCRIPTOR_ENCODE_H
#define PS5VK_DESCRIPTOR_ENCODE_H
#include "vk_pipeline.h"
/* One GFX1013 buffer record shared by the compute and graphics delivery paths.
 * `dynamic_offset` is added to the descriptor's own offset for a dynamic
 * binding and is zero otherwise. Failure leaves the caller's words untouched. */
VkResult ps5vk_buffer_descriptor(VkDevice device,
    const VkDescriptorBufferInfo *info, VkDeviceSize dynamic_offset, uint32_t out[4]);
/* GFX1013 raw storage-buffer table for the audited compiler ABI. No uploads or
 * GPU work. Failure leaves the caller's table untouched. */
VkResult ps5vk_descriptor_encode(VkDevice device,
    const struct ps5vk_compiled_program *program, uint32_t set_index, VkDescriptorSet set,
    const VkDeviceSize dynamic_offsets[PS5VK_MAX_DESCRIPTORS],
    uint32_t *table, size_t capacity_dwords);
#endif

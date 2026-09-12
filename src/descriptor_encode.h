#ifndef PS5VK_DESCRIPTOR_ENCODE_H
#define PS5VK_DESCRIPTOR_ENCODE_H
#include "vk_pipeline.h"
/* GFX1013 raw storage-buffer table for the audited compiler ABI. No uploads or
 * GPU work. Failure leaves the caller's table untouched. */
VkResult ps5vk_descriptor_encode(VkDevice device,
    const struct ps5vk_compiled_program *program, VkDescriptorSet set,
    uint32_t *table, size_t capacity_dwords);
#endif

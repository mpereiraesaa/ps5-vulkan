#ifndef PS5VK_PIPELINE_CACHE_H
#define PS5VK_PIPELINE_CACHE_H
#include "vk_internal.h"

/* Vulkan-visible pipeline cache object.
 *
 * This slice exports the normative 32-byte VkPipelineCacheHeaderVersionOne and
 * nothing else: the implementation keeps no portable compiled-code records yet,
 * so an imported blob is validated and then ignored. Imported data is untrusted
 * and never allocated from, adopted or trusted for identity. */
struct VkPipelineCache_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    uint8_t uuid[VK_UUID_SIZE]; /* compatibility identity this cache belongs to */
    struct VkPipelineCache_T *next;
};

/* True when the handle is a live cache owned by this device. Used by both
 * pipeline-creation entry points and by the merge validation. */
int ps5vk_pipeline_cache_usable(VkDevice device, VkPipelineCache cache);

/* Normative 32-byte VkPipelineCacheHeaderVersionOne writer, and the untrusted
 * import check used by vkCreatePipelineCache. */
void ps5vk_pipeline_cache_write_header(const struct VkPipelineCache_T *cache, uint8_t out[32]);
int ps5vk_pipeline_cache_header_recognized(const struct VkPipelineCache_T *cache,
                                           const void *data, size_t size);

#endif

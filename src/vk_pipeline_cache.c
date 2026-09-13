/*
 * Vulkan 1.0 pipeline cache: object lifecycle, the normative header export and
 * untrusted-input handling.
 *
 * Scope of this slice (deliberately bounded):
 *   - vkCreatePipelineCache / vkDestroyPipelineCache
 *   - vkGetPipelineCacheData exporting exactly the 32-byte
 *     VkPipelineCacheHeaderVersionOne
 *   - vkMergePipelineCaches as a validated no-op
 *   - accepting a live same-device cache in compute and graphics pipeline
 *     creation
 *
 * No compiled-code records are serialized and no restored cache hit is ever
 * reported: executable code still comes from the bounded internal compilation
 * cache on every pipeline creation.
 */
#include "vk_pipeline_cache.h"
#include "compilation_cache.h"
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN

/* Normative VkPipelineCacheHeaderVersionOne layout (Vulkan 1.0):
 *   uint32 headerSize; uint32 headerVersion; uint32 vendorID; uint32 deviceID;
 *   uint8  pipelineCacheUUID[VK_UUID_SIZE];
 * The registry notes the C struct packing is non-normative, so the bytes are
 * written explicitly in little-endian order. */
enum { PS5VK_PIPELINE_CACHE_HEADER_BYTES = 32, PS5VK_PIPELINE_CACHE_HEADER_VERSION = 1 };

/* The exported blob carries only the header today. Any record format added
 * later must bump the format counter below and keep this parser total. */
_Static_assert(PS5VK_COMPILER_VERSION_1 == 1, "uuid derivation input drifted");
_Static_assert(PS5VK_CACHE_ABI_VERSION_1 == 1, "uuid derivation input drifted");

static void store_u32le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t load_u32le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static const VkPhysicalDeviceProperties *device_properties(VkDevice d)
{
    return d && d->physical ? &d->physical->platform.properties : NULL;
}

void ps5vk_pipeline_cache_write_header(const struct VkPipelineCache_T *cache, uint8_t out[32])
{
    store_u32le(out + 0, PS5VK_PIPELINE_CACHE_HEADER_BYTES);
    store_u32le(out + 4, PS5VK_PIPELINE_CACHE_HEADER_VERSION);
    store_u32le(out + 8, cache->device->physical->platform.properties.vendorID);
    store_u32le(out + 12, cache->device->physical->platform.properties.deviceID);
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) out[16 + i] = cache->uuid[i];
}

/* Validate a caller-supplied blob against this cache's identity. Returns 1 when
 * the header is recognized. Unrecognized, short or corrupt data is ignored, as
 * the pinned contract requires: creation still succeeds with an empty cache. */
int ps5vk_pipeline_cache_header_recognized(const struct VkPipelineCache_T *cache,
                                           const void *data, size_t size)
{
    if (!cache || !data || size < PS5VK_PIPELINE_CACHE_HEADER_BYTES) return 0;
    const uint8_t *bytes = (const uint8_t *)data;
    const VkPhysicalDeviceProperties *properties = device_properties(cache->device);
    if (!properties) return 0;
    if (load_u32le(bytes + 0) != PS5VK_PIPELINE_CACHE_HEADER_BYTES) return 0;
    if (load_u32le(bytes + 4) != PS5VK_PIPELINE_CACHE_HEADER_VERSION) return 0;
    if (load_u32le(bytes + 8) != properties->vendorID) return 0;
    if (load_u32le(bytes + 12) != properties->deviceID) return 0;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
        if (bytes[16 + i] != cache->uuid[i]) return 0;
    return 1;
}

int ps5vk_pipeline_cache_usable(VkDevice device, VkPipelineCache cache)
{
    return device && cache && cache->device == device;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice d,
    const VkPipelineCacheCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkPipelineCache *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO ||
        info->pNext || info->flags) return INVALID;
    /* initialDataSize == 0 with a non-NULL pointer is valid input. */
    if (info->initialDataSize && !info->pInitialData) return INVALID;
    const VkPhysicalDeviceProperties *properties = device_properties(d);
    if (!properties) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkPipelineCache cache = ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, allocator, sizeof(*cache),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!cache) return VK_ERROR_OUT_OF_HOST_MEMORY;
    cache->device = d;
    cache->allocator = saved;
    cache->custom_allocator = custom;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
        cache->uuid[i] = properties->pipelineCacheUUID[i];
    /* Untrusted initial data: recognized headers round-trip, everything else is
     * ignored. Nothing is allocated from the blob and no pointer is adopted. */
    (void)ps5vk_pipeline_cache_header_recognized(cache, info->pInitialData,
                                                (size_t)info->initialDataSize);
    cache->next = d->pipeline_caches;
    d->pipeline_caches = cache;
    *out = cache;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice d, VkPipelineCache cache,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!d || !cache || cache->device != d) return;
    VkPipelineCache *link = &d->pipeline_caches;
    while (*link && *link != cache) link = &(*link)->next;
    if (!*link) return;
    *link = cache->next;
    VkAllocationCallbacks saved = cache->allocator; VkBool32 custom = cache->custom_allocator;
    ps5vk_object_free(cache, &saved, custom);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice d, VkPipelineCache cache,
    size_t *data_size, void *data)
{
    if (!d || !cache || cache->device != d || !data_size) return INVALID;
    if (!data) { *data_size = PS5VK_PIPELINE_CACHE_HEADER_BYTES; return VK_SUCCESS; }
    if (*data_size < PS5VK_PIPELINE_CACHE_HEADER_BYTES) {
        /* Contract: nothing is written and the required size is reported as 0. */
        *data_size = 0;
        return VK_INCOMPLETE;
    }
    uint8_t header[PS5VK_PIPELINE_CACHE_HEADER_BYTES];
    ps5vk_pipeline_cache_write_header(cache, header);
    memcpy(data, header, sizeof(header));
    *data_size = PS5VK_PIPELINE_CACHE_HEADER_BYTES;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice d, VkPipelineCache dst,
    uint32_t src_count, const VkPipelineCache *srcs)
{
    if (!d || !dst || dst->device != d || (src_count && !srcs)) return INVALID;
    /* Validate the whole call before touching the destination: self-merge and
     * duplicate sources are legal, foreign or destroyed handles are not. */
    for (uint32_t i = 0; i < src_count; ++i)
        if (!srcs[i] || srcs[i]->device != d) return INVALID;
    /* This slice stores no records, so a validated merge has nothing to copy and
     * the destination is deliberately left unchanged. */
    return VK_SUCCESS;
}

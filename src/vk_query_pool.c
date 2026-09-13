/*
 * Vulkan 1.0 query pool object surface.
 *
 * Scope of this slice: vkCreateQueryPool, vkDestroyQueryPool and
 * vkGetQueryPoolResults. The device-side query commands
 * (vkCmdResetQueryPool, vkCmdBeginQuery, vkCmdEndQuery, vkCmdWriteTimestamp,
 * vkCmdCopyQueryPoolResults) belong to the command-recording file that another
 * task owns, so no query can become available yet. The result query therefore
 * reports VK_NOT_READY and writes nothing: availability is never fabricated.
 *
 * Only VK_QUERY_TYPE_OCCLUSION is created, because occlusion queries are
 * mandatory core and ungated, while timestamps and pipeline statistics require
 * feature bits this implementation reports as false.
 */
#include "vk_query_pool.h"

#define INVALID VK_ERROR_UNKNOWN

VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice d,
    const VkQueryPoolCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkQueryPool *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO ||
        info->pNext || info->flags || !info->queryCount)
        return INVALID;
    if (info->queryCount > PS5VK_MAX_QUERY_POOL_QUERIES)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    switch (info->queryType) {
    case VK_QUERY_TYPE_OCCLUSION:
        /* Core, ungated. pipelineStatistics must not be supplied for it. */
        if (info->pipelineStatistics != 0) return INVALID;
        break;
    case VK_QUERY_TYPE_TIMESTAMP:
        /* timestampQuery is reported false; creating the pool would imply it. */
        return VK_ERROR_FEATURE_NOT_PRESENT;
    case VK_QUERY_TYPE_PIPELINE_STATISTICS:
        /* pipelineStatisticsQuery is reported false. */
        return VK_ERROR_FEATURE_NOT_PRESENT;
    default:
        return INVALID;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkQueryPool pool = ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, allocator, sizeof(*pool),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->device = d;
    pool->allocator = saved;
    pool->custom_allocator = custom;
    pool->query_type = info->queryType;
    pool->query_count = info->queryCount;
    pool->next = d->query_pools;
    d->query_pools = pool;
    *out = pool;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice d, VkQueryPool pool,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!d || !pool || pool->device != d) return;
    VkQueryPool *link = &d->query_pools;
    while (*link && *link != pool) link = &(*link)->next;
    if (!*link) return;
    *link = pool->next;
    VkAllocationCallbacks saved = pool->allocator; VkBool32 custom = pool->custom_allocator;
    ps5vk_object_free(pool, &saved, custom);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d, VkQueryPool pool,
    uint32_t first_query, uint32_t query_count, size_t data_size,
    void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    if (!d || !pool || pool->device != d || !query_count) return INVALID;
    if (flags & ~(VkQueryResultFlags)(VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT))
        return INVALID;
    if (first_query > pool->query_count || query_count > pool->query_count - first_query)
        return INVALID;
    const VkDeviceSize element = (flags & VK_QUERY_RESULT_64_BIT) ? 8u : 4u;
    if (stride == 0) stride = element;
    if (stride % element) return INVALID;
    if (query_count > 1 && stride < element) return INVALID;
    if (!data) {
        if (data_size) return INVALID;
    } else if (data_size < stride * (query_count - 1) + element) {
        return INVALID;
    }
    /* Device-side query writes are not implemented in this profile, so no query
     * is ever available. Returning VK_NOT_READY (rather than VK_SUCCESS with
     * zeros) keeps the contract honest and never fabricates a result. The
     * destination buffer is deliberately left untouched. */
    return VK_NOT_READY;
}

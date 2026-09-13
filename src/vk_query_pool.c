/*
 * Vulkan 1.0 query pool object surface.
 *
 * Scope of this slice: vkCreateQueryPool and vkDestroyQueryPool. The
 * result-retrieval and device-side query commands
 * (vkCmdResetQueryPool, vkCmdBeginQuery, vkCmdEndQuery, vkCmdWriteTimestamp,
 * vkCmdCopyQueryPoolResults and vkGetQueryPoolResults) are deferred as one
 * semantic slice: without initialization and availability state there is no
 * valid vkGetQueryPoolResults call to implement or test.
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

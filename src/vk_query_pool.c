/*
 * Vulkan 1.0 query pool object surface.
 *
 * This bounded slice additionally implements ordered reset and the observable
 * reset-but-unavailable vkGetQueryPoolResults state.  Occlusion begin/end and
 * result copies remain fail-closed until the native backend exposes a measured
 * ZPASS counter.  Timestamp recording remains fail-closed because the queue
 * advertises timestampValidBits=0; CPU time is not a GPU timestamp.
 *
 * Only VK_QUERY_TYPE_OCCLUSION is created, because occlusion queries are
 * mandatory core and ungated. Timestamp pools are refused because the queue
 * reports timestampValidBits=0; pipelineStatisticsQuery is reported false.
 */
#include "vk_query_pool.h"

#include <string.h>

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
        /* The sole queue family reports timestampValidBits=0. */
        return VK_ERROR_FEATURE_NOT_PRESENT;
    case VK_QUERY_TYPE_PIPELINE_STATISTICS:
        /* pipelineStatisticsQuery is reported false. */
        return VK_ERROR_FEATURE_NOT_PRESENT;
    default:
        return INVALID;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkQueryPool pool = ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, allocator,
        sizeof(*pool) + info->queryCount * sizeof(pool->states[0]),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->device = d;
    pool->allocator = saved;
    pool->custom_allocator = custom;
    pool->query_type = info->queryType;
    pool->query_count = info->queryCount;
    memset(pool->states, PS5VK_QUERY_UNINITIALIZED,
        info->queryCount * sizeof(pool->states[0]));
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
    if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_QUERY_POOL, pool)) {
        ++d->lifetime_errors;
        return;
    }
    VkQueryPool *link = &d->query_pools;
    while (*link && *link != pool) link = &(*link)->next;
    if (!*link) return;
    *link = pool->next;
    VkAllocationCallbacks saved = pool->allocator; VkBool32 custom = pool->custom_allocator;
    ps5vk_object_free(pool, &saved, custom);
}

static VkBool32 query_range(VkDevice d, VkQueryPool pool, uint32_t first,
    uint32_t count)
{
    return d && pool && pool->device == d && first < pool->query_count &&
        count <= pool->query_count - first;
}

VkBool32 ps5vk_query_operation(enum ps5vk_operation_type type)
{
    return type == PS5VK_QUERY_RESET;
}

VkResult ps5vk_query_operation_validate(VkDevice d,
    const struct ps5vk_operation *op)
{
    if (!d || !op || op->type != PS5VK_QUERY_RESET ||
        !query_range(d, op->query_pool, op->query_first, op->query_count))
        return INVALID;
    return VK_SUCCESS;
}

VkResult ps5vk_query_operation_execute(VkDevice d,
    const struct ps5vk_operation *op)
{
    VkResult result = ps5vk_query_operation_validate(d, op);
    if (result != VK_SUCCESS) return result;
    memset(&op->query_pool->states[op->query_first], PS5VK_QUERY_UNAVAILABLE,
        op->query_count * sizeof(op->query_pool->states[0]));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer command,
    VkQueryPool pool, uint32_t first, uint32_t count)
{
    VkDevice d = command && command->pool ? command->pool->device : NULL;
    if (!query_range(d, pool, first, count)) {
        ps5vk_command_invalidate(command);
        return;
    }
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(command,
        PS5VK_QUERY_RESET, PS5VK_OPERATION_OUTSIDE_RENDER_PASS, 1);
    if (!op) return;
    op->query_pool = pool;
    op->query_first = first;
    op->query_count = count;
}

static void unsupported_query_command(VkCommandBuffer command)
{
    ps5vk_command_invalidate(command);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer command,
    VkQueryPool pool, uint32_t query, VkQueryControlFlags flags)
{
    (void)pool; (void)query; (void)flags;
    unsupported_query_command(command);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer command,
    VkQueryPool pool, uint32_t query)
{
    (void)pool; (void)query;
    unsupported_query_command(command);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer command,
    VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer destination,
    VkDeviceSize offset, VkDeviceSize stride, VkQueryResultFlags flags)
{
    (void)pool; (void)first; (void)count; (void)destination;
    (void)offset; (void)stride; (void)flags;
    unsupported_query_command(command);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer command,
    VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query)
{
    (void)stage; (void)pool; (void)query;
    unsupported_query_command(command);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d,
    VkQueryPool pool, uint32_t first, uint32_t count, size_t data_size,
    void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    const VkQueryResultFlags allowed = VK_QUERY_RESULT_64_BIT |
        VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    if (!query_range(d, pool, first, count) || d->lost || (flags & ~allowed) ||
        (count && !data))
        return d && d->lost ? VK_ERROR_DEVICE_LOST : INVALID;
    const size_t word = (flags & VK_QUERY_RESULT_64_BIT) ? 8u : 4u;
    const size_t record = word *
        ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ? 2u : 1u);
    if ((count && data_size < record) ||
        (count > 1 && (!stride || stride < record || stride % word ||
            (count - 1u) > (SIZE_MAX - record) / (size_t)stride ||
            data_size < (count - 1u) * (size_t)stride + record)))
        return INVALID;
    for (uint32_t j = 0; j < count; ++j)
        if (pool->states[first + j] == PS5VK_QUERY_UNINITIALIZED) return INVALID;
    if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
        for (uint32_t j = 0; j < count; ++j) {
            uint8_t *record_data = (uint8_t *)data + (size_t)j * (size_t)stride;
            memset(record_data + word, 0, word);
        }
    }
    return count ? VK_NOT_READY : VK_SUCCESS;
}

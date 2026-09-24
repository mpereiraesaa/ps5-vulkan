/*
 * Vulkan 1.0 query pool object surface.
 *
 * Occlusion begin/end are ordered inline render-pass operations. The native
 * backend publishes a result only after exact GPU completion; reset and copy
 * are ordered frontend operations. Timestamp recording remains fail-closed
 * because timestampValidBits=0; CPU time is not a GPU timestamp.
 *
 * Only VK_QUERY_TYPE_OCCLUSION is created, because occlusion queries are
 * mandatory core and ungated. Timestamp pools are refused because the queue
 * reports timestampValidBits=0; pipelineStatisticsQuery is reported false.
 */
#include "vk_query_pool.h"

#include <string.h>

#if defined(PS5VK_OCCLUSION_PRECISE_DIAGNOSTIC) && PS5VK_OCCLUSION_PRECISE_DIAGNOSTIC
#include "ps5log.h"
#endif

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
    size_t state_bytes = info->queryCount * sizeof(uint8_t);
    size_t value_offset = (sizeof(struct VkQueryPool_T) + state_bytes + 7u) & ~(size_t)7u;
    size_t allocation_bytes = value_offset +
        (size_t)info->queryCount * sizeof(uint64_t);
    VkQueryPool pool = ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, allocator,
        allocation_bytes,
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->device = d;
    pool->allocator = saved;
    pool->custom_allocator = custom;
    pool->query_type = info->queryType;
    pool->query_count = info->queryCount;
    pool->values = (uint64_t *)((uint8_t *)pool + value_offset);
    memset(pool->states, PS5VK_QUERY_UNINITIALIZED,
        info->queryCount * sizeof(pool->states[0]));
    memset(pool->values, 0, (size_t)info->queryCount * sizeof(pool->values[0]));
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

static VkBool32 query_range_available(VkQueryPool pool, uint32_t first,
    uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        if (pool->states[first + i] != PS5VK_QUERY_AVAILABLE) return VK_FALSE;
    return VK_TRUE;
}

VkBool32 ps5vk_query_operation(enum ps5vk_operation_type type)
{
    return type == PS5VK_QUERY_RESET || type == PS5VK_QUERY_BEGIN ||
        type == PS5VK_QUERY_END || type == PS5VK_QUERY_COPY;
}

static VkBool32 query_result_layout(const struct ps5vk_operation *op,
    size_t *record_bytes, VkDeviceSize *total_bytes)
{
    const VkQueryResultFlags allowed = VK_QUERY_RESULT_64_BIT |
        VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
        VK_QUERY_RESULT_PARTIAL_BIT;
    size_t word = (op->query_flags & VK_QUERY_RESULT_64_BIT) ? 8u : 4u;
    size_t record = word * ((op->query_flags &
        VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ? 2u : 1u);
    /* A zero stride is useful for copying one result at a time to adjacent
     * offsets. The pinned CTS uses exactly that form; with one query the
     * stride is unused. Keep multi-query zero-stride copies fail-closed since
     * overlapping result writes are not exercised by this backend contract. */
    if (!op->query_count || (op->query_flags & ~allowed) ||
        (op->query_stride == 0 && op->query_count != 1) ||
        (op->query_stride != 0 &&
            (op->query_stride < record || op->query_stride % word)) ||
        (op->query_stride != 0 && (op->query_count - 1u) >
            (UINT64_MAX - record) / op->query_stride))
        return VK_FALSE;
    *record_bytes = record;
    *total_bytes = (op->query_count - 1u) * op->query_stride + record;
    return VK_TRUE;
}

VkResult ps5vk_query_operation_validate(VkDevice d,
    const struct ps5vk_operation *op)
{
    if (!d || !op || !query_range(d, op->query_pool, op->query_first, op->query_count))
        return INVALID;
    if (op->type == PS5VK_QUERY_RESET) return VK_SUCCESS;
    if (op->type == PS5VK_QUERY_COPY) {
        size_t record_bytes;
        VkDeviceSize total_bytes;
        void *address;
        VkDeviceSize span;
        if (!query_result_layout(op, &record_bytes, &total_bytes) ||
            !op->copy_destination ||
            op->buffer_offset % ((op->query_flags & VK_QUERY_RESULT_64_BIT) ? 8u : 4u) ||
            ps5vk_buffer_span(d, op->copy_destination, op->buffer_offset,
                total_bytes, &address, &span) != VK_SUCCESS || span < total_bytes)
            return INVALID;
        (void)record_bytes;
        return VK_SUCCESS;
    }
    if ((op->type != PS5VK_QUERY_BEGIN && op->type != PS5VK_QUERY_END) ||
        op->query_count != 1 || op->query_flags & ~VK_QUERY_CONTROL_PRECISE_BIT)
        return INVALID;
    if ((op->query_flags & VK_QUERY_CONTROL_PRECISE_BIT) &&
        !(d->enabled_features & PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    return VK_SUCCESS;
}

VkResult ps5vk_query_operation_execute(VkDevice d,
    const struct ps5vk_operation *op)
{
    VkResult result = ps5vk_query_operation_validate(d, op);
    if (result != VK_SUCCESS) return result;
    if (op->type == PS5VK_QUERY_RESET) {
        memset(&op->query_pool->states[op->query_first], PS5VK_QUERY_UNAVAILABLE,
            op->query_count * sizeof(op->query_pool->states[0]));
        memset(&op->query_pool->values[op->query_first], 0,
            op->query_count * sizeof(op->query_pool->values[0]));
        return VK_SUCCESS;
    }
    if (op->type != PS5VK_QUERY_COPY) return VK_ERROR_FEATURE_NOT_PRESENT;
    size_t record_bytes;
    VkDeviceSize total_bytes;
    void *address;
    VkDeviceSize span;
    if (!query_result_layout(op, &record_bytes, &total_bytes) ||
        ps5vk_buffer_span(d, op->copy_destination, op->buffer_offset,
            total_bytes, &address, &span) != VK_SUCCESS || span < total_bytes)
        return INVALID;
    size_t word = (op->query_flags & VK_QUERY_RESULT_64_BIT) ? 8u : 4u;
    for (uint32_t j = 0; j < op->query_count; ++j) {
        uint8_t *record = (uint8_t *)address + (size_t)j * (size_t)op->query_stride;
        VkBool32 available = op->query_pool->states[op->query_first + j] ==
            PS5VK_QUERY_AVAILABLE;
        if (available || (op->query_flags & VK_QUERY_RESULT_PARTIAL_BIT)) {
            /* Zero is a valid intermediate value for an unavailable query. */
            uint64_t value = available ?
                op->query_pool->values[op->query_first + j] : 0;
            if (word == 8u) memcpy(record, &value, sizeof(value));
            else { uint32_t low = (uint32_t)value; memcpy(record, &low, sizeof(low)); }
        }
        if (op->query_flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
            uint64_t value = available ? 1u : 0u;
            if (word == 8u) memcpy(record + word, &value, sizeof(value));
            else { uint32_t low = (uint32_t)value; memcpy(record + word, &low, sizeof(low)); }
        }
    }
    (void)record_bytes;
    return VK_SUCCESS;
}

VkResult ps5vk_query_publish(VkDevice d, VkQueryPool pool,
    uint32_t query, uint64_t value)
{
    if (!d || !pool || pool->device != d || pool->query_type != VK_QUERY_TYPE_OCCLUSION ||
        query >= pool->query_count || pool->states[query] == PS5VK_QUERY_UNINITIALIZED)
        return INVALID;
    pool->values[query] = value;
    pool->states[query] = PS5VK_QUERY_AVAILABLE;
    return VK_SUCCESS;
}

VkBool32 ps5vk_query_reset_before(VkCommandBuffer command,
    uint32_t operation_index, VkQueryPool pool, uint32_t query)
{
    if (!command || !pool || query >= pool->query_count ||
        operation_index > command->operation_count)
        return VK_FALSE;
    VkBool32 reset = pool->states[query] == PS5VK_QUERY_UNAVAILABLE;
    for (uint32_t i = 0; i < operation_index; ++i) {
        const struct ps5vk_operation *op = &command->operations[i];
        if (op->query_pool != pool) continue;
        if (op->type == PS5VK_QUERY_RESET && query >= op->query_first &&
            query - op->query_first < op->query_count) {
            reset = VK_TRUE;
        } else if ((op->type == PS5VK_QUERY_BEGIN || op->type == PS5VK_QUERY_END) &&
            op->query_first == query) {
            reset = VK_FALSE;
        }
    }
    return reset;
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

static void unsupported_query_command(VkCommandBuffer command, unsigned site)
{
    (void)site;
#if defined(PS5VK_OCCLUSION_PRECISE_DIAGNOSTIC) && PS5VK_OCCLUSION_PRECISE_DIAGNOSTIC
    ps5log_printf(PS5LOG_MARK, "PS5VK_QUERY_RECORD_REFUSE site=%u", site);
#endif
    ps5vk_command_invalidate(command);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer command,
    VkQueryPool pool, uint32_t query, VkQueryControlFlags flags)
{
    VkDevice d = command && command->pool ? command->pool->device : NULL;
    if (!d || !pool || pool->device != d || pool->query_type != VK_QUERY_TYPE_OCCLUSION ||
        query >= pool->query_count || !command->render_pass ||
        command->active_occlusion_query_pool || (flags & ~VK_QUERY_CONTROL_PRECISE_BIT) ||
        /* A continuation secondary may begin its own local query. Its reset
         * can only be recorded in the primary before the render pass, so the
         * full ordered reset check is made when the primary is submitted. */
        (!(command->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
           command->render_pass_inherited) &&
         !ps5vk_query_reset_before(command, command->operation_count, pool, query)) ||
        ((flags & VK_QUERY_CONTROL_PRECISE_BIT) &&
         !(d->enabled_features & PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE))) {
        unsupported_query_command(command, 1);
        return;
    }
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(command,
        PS5VK_QUERY_BEGIN, PS5VK_OPERATION_INSIDE_RENDER_PASS, 1);
    if (!op) return;
    op->query_pool = pool;
    op->query_first = query;
    op->query_count = 1;
    op->query_flags = flags;
    op->render_pass = command->render_pass;
    op->framebuffer = command->framebuffer;
    op->subpass = command->subpass;
    command->active_occlusion_query_pool = pool;
    command->active_occlusion_query = query;
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer command,
    VkQueryPool pool, uint32_t query)
{
    if (!command || !command->active_occlusion_query_pool ||
        command->active_occlusion_query_pool != pool ||
        command->active_occlusion_query != query) {
        unsupported_query_command(command, 2);
        return;
    }
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(command,
        PS5VK_QUERY_END, PS5VK_OPERATION_INSIDE_RENDER_PASS, 1);
    if (!op) return;
    op->query_pool = pool;
    op->query_first = query;
    op->query_count = 1;
    op->query_flags = 0;
    op->render_pass = command->render_pass;
    op->framebuffer = command->framebuffer;
    op->subpass = command->subpass;
    command->active_occlusion_query_pool = VK_NULL_HANDLE;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer command,
    VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer destination,
    VkDeviceSize offset, VkDeviceSize stride, VkQueryResultFlags flags)
{
    VkDevice d = command && command->pool ? command->pool->device : NULL;
    if (!count) return;
    if (!query_range(d, pool, first, count) || !destination ||
        command->render_pass) {
        unsupported_query_command(command, 3);
        return;
    }
    struct ps5vk_operation candidate = {
        .type = PS5VK_QUERY_COPY,
        .query_pool = pool,
        .query_first = first,
        .query_count = count,
        .query_flags = (VkQueryControlFlags)flags,
        .query_stride = stride,
        .copy_destination = destination,
        .buffer_offset = offset,
    };
    if (ps5vk_query_operation_validate(d, &candidate) != VK_SUCCESS) {
        unsupported_query_command(command, 4);
        return;
    }
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(command,
        PS5VK_QUERY_COPY, PS5VK_OPERATION_OUTSIDE_RENDER_PASS, 1);
    if (op) *op = candidate;
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer command,
    VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query)
{
    (void)stage; (void)pool; (void)query;
    unsupported_query_command(command, 5);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d,
    VkQueryPool pool, uint32_t first, uint32_t count, size_t data_size,
    void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    const VkQueryResultFlags allowed = VK_QUERY_RESULT_64_BIT |
        VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
        VK_QUERY_RESULT_PARTIAL_BIT;
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
    if (flags & VK_QUERY_RESULT_WAIT_BIT) {
        while (!query_range_available(pool, first, count)) {
            VkBool32 uninitialized = VK_FALSE;
            for (uint32_t j = 0; j < count; ++j)
                uninitialized |= pool->states[first + j] == PS5VK_QUERY_UNINITIALIZED;
            if (!d->submission) return uninitialized ? INVALID : VK_NOT_READY;
            if (!d->progress.poll || !d->progress.pause) return INVALID;
            VkResult progress = d->progress.poll(d);
            if (progress != VK_SUCCESS) return progress;
            if (d->lost) return VK_ERROR_DEVICE_LOST;
            if (!query_range_available(pool, first, count)) {
                if (!d->submission) {
                    for (uint32_t j = 0; j < count; ++j)
                        if (pool->states[first + j] == PS5VK_QUERY_UNINITIALIZED)
                            return INVALID;
                    return VK_NOT_READY;
                }
                d->progress.pause(d->progress.context, UINT64_C(300000000));
            }
        }
    }
    for (uint32_t j = 0; j < count; ++j)
        if (pool->states[first + j] == PS5VK_QUERY_UNINITIALIZED) return INVALID;
    VkBool32 all_available = VK_TRUE;
    for (uint32_t j = 0; j < count; ++j) {
        uint8_t *record_data = (uint8_t *)data + (size_t)j * (size_t)stride;
        VkBool32 available = pool->states[first + j] == PS5VK_QUERY_AVAILABLE;
        if (!available) all_available = VK_FALSE;
        if (available || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) {
            /* Return a legal zero intermediate until the GPU publishes its
             * completed sample count. */
            uint64_t value = available ? pool->values[first + j] : 0;
            if (word == 8u) memcpy(record_data, &value, sizeof(value));
            else { uint32_t low = (uint32_t)value; memcpy(record_data, &low, sizeof(low)); }
        }
        if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
            uint64_t availability = available ? 1u : 0u;
            if (word == 8u) memcpy(record_data + word, &availability, sizeof(availability));
            else { uint32_t low = (uint32_t)availability; memcpy(record_data + word, &low, sizeof(low)); }
        }
    }
    return all_available ? VK_SUCCESS : VK_NOT_READY;
}

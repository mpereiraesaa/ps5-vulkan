#include "vk_buffer_transfer.h"
#include <string.h>

VkBool32 ps5vk_buffer_transfer_operation(enum ps5vk_operation_type type)
{
    return type == PS5VK_COPY_BUFFER || type == PS5VK_UPDATE_BUFFER ||
        type == PS5VK_FILL_BUFFER;
}

VkResult ps5vk_buffer_transfer_validate(VkDevice d,
    const struct ps5vk_operation *op)
{
    if (!d || !op || !ps5vk_buffer_transfer_operation(op->type))
        return VK_ERROR_UNKNOWN;
    void *address;
    VkDeviceSize bytes;
    if (op->type == PS5VK_COPY_BUFFER) {
        if (!ps5vk_buffer_usage(d, op->copy_source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
            !ps5vk_buffer_usage(d, op->copy_destination, VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
            ps5vk_buffer_span(d, op->copy_source, op->buffer_copy.srcOffset,
                op->buffer_copy.size, &address, &bytes) != VK_SUCCESS ||
            ps5vk_buffer_span(d, op->copy_destination, op->buffer_copy.dstOffset,
                op->buffer_copy.size, &address, &bytes) != VK_SUCCESS)
            return VK_ERROR_UNKNOWN;
        return VK_SUCCESS;
    }
    if (!ps5vk_buffer_usage(d, op->copy_destination,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !op->buffer_size ||
        ps5vk_buffer_span(d, op->copy_destination, op->buffer_offset,
            op->buffer_size, &address, &bytes) != VK_SUCCESS)
        return VK_ERROR_UNKNOWN;
    if (op->type == PS5VK_UPDATE_BUFFER &&
        (!op->owned_payload || op->owned_payload_size != op->buffer_size))
        return VK_ERROR_UNKNOWN;
    if (op->type == PS5VK_FILL_BUFFER && (op->buffer_size & 3u))
        return VK_ERROR_UNKNOWN;
    return VK_SUCCESS;
}

VkResult ps5vk_buffer_transfer_execute(VkDevice d,
    const struct ps5vk_operation *op)
{
    VkResult rc = ps5vk_buffer_transfer_validate(d, op);
    if (rc != VK_SUCCESS) return rc;
    void *source = NULL, *destination = NULL;
    VkDeviceSize bytes = 0;
    if (op->type == PS5VK_COPY_BUFFER) {
        rc = ps5vk_buffer_span(d, op->copy_source, op->buffer_copy.srcOffset,
            op->buffer_copy.size, &source, &bytes);
        if (rc != VK_SUCCESS) return rc;
        rc = ps5vk_buffer_span(d, op->copy_destination, op->buffer_copy.dstOffset,
            op->buffer_copy.size, &destination, &bytes);
        if (rc != VK_SUCCESS) return rc;
        rc = ps5vk_buffer_cache(d, op->copy_source, op->buffer_copy.srcOffset,
            op->buffer_copy.size, VK_TRUE);
        if (rc != VK_SUCCESS) return rc;
        memcpy(destination, source, (size_t)op->buffer_copy.size);
        return ps5vk_buffer_cache(d, op->copy_destination,
            op->buffer_copy.dstOffset, op->buffer_copy.size, VK_FALSE);
    }
    rc = ps5vk_buffer_span(d, op->copy_destination, op->buffer_offset,
        op->buffer_size, &destination, &bytes);
    if (rc != VK_SUCCESS) return rc;
    if (op->type == PS5VK_UPDATE_BUFFER)
        memcpy(destination, op->owned_payload, (size_t)op->buffer_size);
    else {
        uint32_t *words = destination;
        for (VkDeviceSize j = 0; j < op->buffer_size / 4; ++j)
            words[j] = op->fill_data;
    }
    return ps5vk_buffer_cache(d, op->copy_destination, op->buffer_offset,
        op->buffer_size, VK_FALSE);
}

#include "vk_indirect.h"

#include <string.h>

#define INVALID VK_ERROR_UNKNOWN

VkBool32 ps5vk_indirect_compute_operation(enum ps5vk_operation_type type)
{ return type == PS5VK_DISPATCH_INDIRECT; }

VkBool32 ps5vk_indirect_graphics_operation(enum ps5vk_operation_type type)
{
    return type == PS5VK_DRAW_INDIRECT || type == PS5VK_DRAW_INDEXED_INDIRECT ||
        type == PS5VK_DRAW_INDIRECT_BYTE_COUNT;
}

VkBool32 ps5vk_indirect_operation(enum ps5vk_operation_type type)
{ return ps5vk_indirect_compute_operation(type) || ps5vk_indirect_graphics_operation(type); }

size_t ps5vk_indirect_argument_size(enum ps5vk_operation_type type)
{
    if (type == PS5VK_DISPATCH_INDIRECT) return sizeof(VkDispatchIndirectCommand);
    if (type == PS5VK_DRAW_INDIRECT) return sizeof(VkDrawIndirectCommand);
    if (type == PS5VK_DRAW_INDEXED_INDIRECT) return sizeof(VkDrawIndexedIndirectCommand);
    /* The transform feedback counter: one dword. */
    if (type == PS5VK_DRAW_INDIRECT_BYTE_COUNT) return sizeof(uint32_t);
    return 0;
}

VkBool32 ps5vk_indirect_argument_span(enum ps5vk_operation_type type, uint32_t count,
                                      uint32_t stride, VkDeviceSize *length)
{
    const size_t bytes = ps5vk_indirect_argument_size(type);
    if (!bytes || !length) return VK_FALSE;
    /* A dispatch is always exactly one structure. */
    if (ps5vk_indirect_compute_operation(type)) {
        if (count != 1) return VK_FALSE;
        *length = bytes;
        return VK_TRUE;
    }
    if (!count) { *length = 0; return VK_TRUE; }
    /* drawCount == 1: the stride is not read and imposes no rule. */
    if (count == 1) { *length = bytes; return VK_TRUE; }
    if ((stride & 3u) || stride < bytes) return VK_FALSE;
    /* (count - 1) * stride < 2^32 * 2^32 cannot overflow 64 bits, but the
     * final addition is still checked rather than assumed. */
    const VkDeviceSize strides = (VkDeviceSize)(count - 1u) * (VkDeviceSize)stride;
    if (strides > (VkDeviceSize)(UINT64_MAX) - bytes) return VK_FALSE;
    *length = strides + bytes;
    return VK_TRUE;
}

/* Where command `index` of the recorded operation lives, with the same checked
 * arithmetic the span used; the caller has already validated the whole span
 * so this cannot name bytes the buffer does not hold. */
static VkBool32 command_offset(const struct ps5vk_operation *op, uint32_t index,
                               VkDeviceSize *offset)
{
    if (index >= op->indirect_count) return VK_FALSE;
    if (op->indirect_count == 1) { *offset = op->indirect_offset; return VK_TRUE; }
    const VkDeviceSize advance = (VkDeviceSize)index * (VkDeviceSize)op->indirect_stride;
    if (advance > UINT64_MAX - op->indirect_offset) return VK_FALSE;
    *offset = op->indirect_offset + advance;
    return VK_TRUE;
}

VkResult ps5vk_indirect_validate(VkDevice d, const struct ps5vk_operation *op)
{
    if (!d || !op || !ps5vk_indirect_operation(op->type) ||
        !ps5vk_buffer_usage(d, op->indirect_buffer,
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) ||
        (op->indirect_offset & 3u)) return INVALID;
    VkDeviceSize length = 0;
    if (!ps5vk_indirect_argument_span(op->type, op->indirect_count,
                                      op->indirect_stride, &length)) return INVALID;
    /* A byte-count draw is exactly one command with a vertex stride inside
     * maxTransformFeedbackBufferDataStride (VUID-02289), on a device that
     * enabled transformFeedback. */
    if (op->type == PS5VK_DRAW_INDIRECT_BYTE_COUNT &&
        (op->indirect_count != 1 || !op->indirect_stride ||
         op->indirect_stride > PS5VK_XFB_BUFFER_DATA_STRIDE ||
         !(d->enabled_features_t09 & PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK)))
        return INVALID;
    if (ps5vk_indirect_graphics_operation(op->type)) {
        /* The physical limit bounds every command; the multiDrawIndirect
         * feature must additionally be ENABLED on this device before a second
         * command may be named, whatever the physical device could do. */
        if (op->indirect_count > d->physical->platform.properties.limits.maxDrawIndirectCount ||
            (op->indirect_count > 1 &&
             !(d->enabled_features & PS5VK_FEATURE_MULTI_DRAW_INDIRECT))) return INVALID;
        if (!op->indirect_count) {
            void *bound = NULL; VkDeviceSize available = 0;
            return ps5vk_buffer_span(d, op->indirect_buffer,
                0, VK_WHOLE_SIZE, &bound, &available);
        }
    }
    void *address = NULL;
    VkDeviceSize available = 0;
    return ps5vk_buffer_span(d, op->indirect_buffer, op->indirect_offset,
                             length, &address, &available);
}

VkResult ps5vk_indirect_resolve_command(VkDevice d, const struct ps5vk_operation *recorded,
                                        uint32_t index, struct ps5vk_operation *resolved)
{
    if (!resolved || ps5vk_indirect_validate(d, recorded) != VK_SUCCESS)
        return INVALID;
    VkDeviceSize offset = 0;
    if (!command_offset(recorded, index, &offset)) return INVALID;
    const size_t bytes = ps5vk_indirect_argument_size(recorded->type);
    /* Exactly this command's bytes become CPU-visible; a neighbouring command
     * or stride padding is never read through this snapshot. */
    VkResult rc = ps5vk_buffer_cache(d, recorded->indirect_buffer, offset, bytes, VK_TRUE);
    if (rc != VK_SUCCESS) return rc;
    void *address = NULL;
    VkDeviceSize available = 0;
    rc = ps5vk_buffer_span(d, recorded->indirect_buffer, offset, bytes, &address, &available);
    if (rc != VK_SUCCESS) return rc;
    struct ps5vk_operation snapshot = *recorded;
    snapshot.draw_index = index;
    if (recorded->type == PS5VK_DISPATCH_INDIRECT) {
        VkDispatchIndirectCommand command;
        memcpy(&command, address, sizeof(command));
        const VkPhysicalDeviceLimits *limits = &d->physical->platform.properties.limits;
        if (command.x > limits->maxComputeWorkGroupCount[0] ||
            command.y > limits->maxComputeWorkGroupCount[1] ||
            command.z > limits->maxComputeWorkGroupCount[2]) return INVALID;
        snapshot.type = PS5VK_DISPATCH;
        snapshot.groups[0] = command.x;
        snapshot.groups[1] = command.y;
        snapshot.groups[2] = command.z;
        *resolved = snapshot;
        return VK_SUCCESS;
    }
    /* drawIndirectFirstInstance: the value is legal only on a device that
     * enabled the feature; a device without it must see zero here, so anything
     * else is refused before it can reach the backend. */
    const VkBool32 first_instance_enabled =
        !!(d->enabled_features & PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE);
    if (recorded->type == PS5VK_DRAW_INDIRECT_BYTE_COUNT) {
        /* vertexCount = (counter - counterOffset) / vertexStride; a counter
         * at or below the offset draws nothing. The instances are the
         * command's own parameters. */
        uint32_t counter;
        memcpy(&counter, address, sizeof(counter));
        snapshot.type = PS5VK_DRAW;
        snapshot.vertex_count = counter > recorded->byte_count_offset ?
            (counter - recorded->byte_count_offset) / recorded->indirect_stride : 0u;
        snapshot.first_vertex = 0;
        snapshot.index_count = 0;
        snapshot.first_index = 0;
        snapshot.vertex_offset = 0;
        *resolved = snapshot;
        return VK_SUCCESS;
    }
    if (recorded->type == PS5VK_DRAW_INDIRECT) {
        VkDrawIndirectCommand command;
        memcpy(&command, address, sizeof(command));
        if (command.firstInstance && !first_instance_enabled) return INVALID;
        snapshot.type = PS5VK_DRAW;
        snapshot.vertex_count = command.vertexCount;
        snapshot.instance_count = command.instanceCount;
        snapshot.first_vertex = command.firstVertex;
        snapshot.first_instance = command.firstInstance;
        snapshot.index_count = 0;
        snapshot.first_index = 0;
        snapshot.vertex_offset = 0;
        *resolved = snapshot;
        return VK_SUCCESS;
    }
    VkDrawIndexedIndirectCommand command;
    memcpy(&command, address, sizeof(command));
    if (command.firstInstance && !first_instance_enabled) return INVALID;
    snapshot.type = PS5VK_DRAW_INDEXED;
    snapshot.index_count = command.indexCount;
    snapshot.instance_count = command.instanceCount;
    snapshot.first_index = command.firstIndex;
    snapshot.vertex_offset = command.vertexOffset;
    snapshot.first_instance = command.firstInstance;
    snapshot.vertex_count = 0;
    snapshot.first_vertex = 0;
    *resolved = snapshot;
    return VK_SUCCESS;
}

VkResult ps5vk_indirect_resolve(VkDevice d, const struct ps5vk_operation *recorded,
                                struct ps5vk_operation *resolved)
{
    if (!resolved || ps5vk_indirect_validate(d, recorded) != VK_SUCCESS)
        return INVALID;
    if (ps5vk_indirect_graphics_operation(recorded->type)) {
        /* A multi-command record has no single snapshot. */
        if (recorded->indirect_count > 1) return INVALID;
        if (!recorded->indirect_count) {
            *resolved = *recorded;
            resolved->type = recorded->type == PS5VK_DRAW_INDIRECT ?
                PS5VK_DRAW : PS5VK_DRAW_INDEXED;
            resolved->vertex_count = resolved->instance_count = 0;
            resolved->index_count = 0;
            resolved->first_vertex = resolved->first_instance = 0;
            resolved->first_index = 0;
            resolved->vertex_offset = 0;
            resolved->draw_index = 0;
            return VK_SUCCESS;
        }
    }
    return ps5vk_indirect_resolve_command(d, recorded, 0, resolved);
}

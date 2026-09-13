#include "vk_indirect.h"

#include <string.h>

#define INVALID VK_ERROR_UNKNOWN

VkBool32 ps5vk_indirect_compute_operation(enum ps5vk_operation_type type)
{ return type == PS5VK_DISPATCH_INDIRECT; }

VkBool32 ps5vk_indirect_graphics_operation(enum ps5vk_operation_type type)
{ return type == PS5VK_DRAW_INDIRECT || type == PS5VK_DRAW_INDEXED_INDIRECT; }

VkBool32 ps5vk_indirect_operation(enum ps5vk_operation_type type)
{ return ps5vk_indirect_compute_operation(type) || ps5vk_indirect_graphics_operation(type); }

static size_t argument_size(enum ps5vk_operation_type type)
{
    if (type == PS5VK_DISPATCH_INDIRECT) return sizeof(VkDispatchIndirectCommand);
    if (type == PS5VK_DRAW_INDIRECT) return sizeof(VkDrawIndirectCommand);
    if (type == PS5VK_DRAW_INDEXED_INDIRECT) return sizeof(VkDrawIndexedIndirectCommand);
    return 0;
}

VkResult ps5vk_indirect_validate(VkDevice d, const struct ps5vk_operation *op)
{
    if (!d || !op || !ps5vk_indirect_operation(op->type) ||
        !ps5vk_buffer_usage(d, op->indirect_buffer,
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) ||
        (op->indirect_offset & 3u)) return INVALID;
    const size_t bytes = argument_size(op->type);
    if (!bytes) return INVALID;
    if (ps5vk_indirect_graphics_operation(op->type)) {
        /* The advertised core feature profile leaves multiDrawIndirect false. */
        if (op->indirect_count > d->physical->platform.properties.limits.maxDrawIndirectCount ||
            op->indirect_count > 1 ||
            (op->indirect_count > 1 && ((op->indirect_stride & 3u) ||
                                        op->indirect_stride < bytes))) return INVALID;
        if (!op->indirect_count) {
            void *bound = NULL; VkDeviceSize available = 0;
            return ps5vk_buffer_span(d, op->indirect_buffer,
                0, VK_WHOLE_SIZE, &bound, &available);
        }
    }
    void *address = NULL;
    VkDeviceSize available = 0;
    return ps5vk_buffer_span(d, op->indirect_buffer, op->indirect_offset,
                             bytes, &address, &available);
}

VkResult ps5vk_indirect_resolve(VkDevice d, const struct ps5vk_operation *recorded,
                                struct ps5vk_operation *resolved)
{
    if (!resolved || ps5vk_indirect_validate(d, recorded) != VK_SUCCESS)
        return INVALID;
    *resolved = *recorded;
    const size_t bytes = argument_size(recorded->type);
    if (ps5vk_indirect_graphics_operation(recorded->type) &&
        !recorded->indirect_count) {
        resolved->type = recorded->type == PS5VK_DRAW_INDIRECT ?
            PS5VK_DRAW : PS5VK_DRAW_INDEXED;
        resolved->vertex_count = resolved->instance_count = 0;
        resolved->index_count = 0;
        resolved->first_vertex = resolved->first_instance = 0;
        resolved->first_index = 0;
        resolved->vertex_offset = 0;
        return VK_SUCCESS;
    }
    VkResult rc = ps5vk_buffer_cache(d, recorded->indirect_buffer,
        recorded->indirect_offset, bytes, VK_TRUE);
    if (rc != VK_SUCCESS) return rc;
    void *address = NULL;
    VkDeviceSize available = 0;
    rc = ps5vk_buffer_span(d, recorded->indirect_buffer,
        recorded->indirect_offset, bytes, &address, &available);
    if (rc != VK_SUCCESS) return rc;
    if (recorded->type == PS5VK_DISPATCH_INDIRECT) {
        VkDispatchIndirectCommand command;
        memcpy(&command, address, sizeof(command));
        const VkPhysicalDeviceLimits *limits = &d->physical->platform.properties.limits;
        if (command.x > limits->maxComputeWorkGroupCount[0] ||
            command.y > limits->maxComputeWorkGroupCount[1] ||
            command.z > limits->maxComputeWorkGroupCount[2]) return INVALID;
        resolved->type = PS5VK_DISPATCH;
        resolved->groups[0] = command.x;
        resolved->groups[1] = command.y;
        resolved->groups[2] = command.z;
        return VK_SUCCESS;
    }
    if (recorded->type == PS5VK_DRAW_INDIRECT) {
        VkDrawIndirectCommand command;
        memcpy(&command, address, sizeof(command));
        /* drawIndirectFirstInstance is not advertised by this profile. */
        if (command.firstInstance) return INVALID;
        resolved->type = PS5VK_DRAW;
        resolved->vertex_count = command.vertexCount;
        resolved->instance_count = command.instanceCount;
        resolved->first_vertex = command.firstVertex;
        resolved->first_instance = command.firstInstance;
        return VK_SUCCESS;
    }
    VkDrawIndexedIndirectCommand command;
    memcpy(&command, address, sizeof(command));
    if (command.firstInstance) return INVALID;
    resolved->type = PS5VK_DRAW_INDEXED;
    resolved->index_count = command.indexCount;
    resolved->instance_count = command.instanceCount;
    resolved->first_index = command.firstIndex;
    resolved->vertex_offset = command.vertexOffset;
    resolved->first_instance = command.firstInstance;
    return VK_SUCCESS;
}

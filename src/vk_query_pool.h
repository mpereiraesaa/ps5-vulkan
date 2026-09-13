#ifndef PS5VK_QUERY_POOL_H
#define PS5VK_QUERY_POOL_H
#include "vk_command.h"

enum ps5vk_query_state {
    PS5VK_QUERY_UNINITIALIZED,
    PS5VK_QUERY_UNAVAILABLE,
};

/* Occlusion-capable query pool object.  Slots deliberately distinguish the
 * Vulkan uninitialized state from a reset-but-unavailable query.  No available
 * state exists until a native occlusion counter can publish a real result. */
struct VkQueryPool_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkQueryType query_type;
    uint32_t query_count;
    struct VkQueryPool_T *next;
    uint8_t states[];
};

/* Bounded resource policy: a query pool larger than this is refused with
 * VK_ERROR_OUT_OF_HOST_MEMORY rather than allocating an unbounded array. */
enum { PS5VK_MAX_QUERY_POOL_QUERIES = 4096 };

VkBool32 ps5vk_query_operation(enum ps5vk_operation_type type);
VkResult ps5vk_query_operation_validate(VkDevice device,
    const struct ps5vk_operation *operation);
VkResult ps5vk_query_operation_execute(VkDevice device,
    const struct ps5vk_operation *operation);

#endif

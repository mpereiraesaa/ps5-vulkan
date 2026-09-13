#ifndef PS5VK_QUERY_POOL_H
#define PS5VK_QUERY_POOL_H
#include "vk_internal.h"

/* Occlusion-capable query pool object.
 *
 * This slice implements the object lifetime surface only. Result retrieval is
 * intentionally deferred with query command recording so the public API does
 * not pretend that uninitialized queries are merely unavailable. */
struct VkQueryPool_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkQueryType query_type;
    uint32_t query_count;
    struct VkQueryPool_T *next;
};

/* Bounded resource policy: a query pool larger than this is refused with
 * VK_ERROR_OUT_OF_HOST_MEMORY rather than allocating an unbounded array. */
enum { PS5VK_MAX_QUERY_POOL_QUERIES = 4096 };

#endif

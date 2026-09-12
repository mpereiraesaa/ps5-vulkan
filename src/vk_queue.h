#ifndef PS5VK_QUEUE_H
#define PS5VK_QUEUE_H
#include "vk_command.h"
#include "vk_fence.h"
enum { PS5VK_MAX_SUBMITTED_BUFFERS = 32 };
struct ps5vk_submission {
    uint64_t serial;
    VkFence fence;
    unsigned count;
    VkCommandBuffer buffers[PS5VK_MAX_SUBMITTED_BUFFERS];
    void *backend_job;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
};
VkResult ps5vk_queue_poll(VkDevice device);
void ps5vk_queue_router_configure(VkDevice, struct ps5vk_queue_backend,
                                struct ps5vk_queue_backend);
#endif

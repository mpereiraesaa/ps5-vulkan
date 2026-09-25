#ifndef PS5VK_QUEUE_H
#define PS5VK_QUEUE_H
#include "vk_command.h"
#include "vk_fence.h"
#include "vk_sync.h"
enum { PS5VK_MAX_SUBMITTED_BUFFERS = 32 };
struct ps5vk_submission {
    uint64_t serial;
    VkFence fence;
    unsigned count;
    VkCommandBuffer buffers[PS5VK_MAX_SUBMITTED_BUFFERS];
    uint16_t first_operation[PS5VK_MAX_SUBMITTED_BUFFERS];
    uint16_t operation_count[PS5VK_MAX_SUBMITTED_BUFFERS];
    VkBool32 frontend_only;
    /* Preparation is delayed until this segment reaches the queue head so
     * indirect parameters observe all prior queue writes. */
    VkBool32 deferred_prepare;
    uint32_t wait_count, signal_count;
    VkSemaphore *waits, *signals;
    /* Timeline values parallel to waits/signals; unused for binary ones. */
    uint64_t *wait_values, *signal_values;
    struct ps5vk_submission *next;
    void *backend_job;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkBool32 reserved;
    VkBool32 waits_consumed;
};
static inline uint32_t ps5vk_submission_first_operation(
    const struct ps5vk_submission *submission, uint32_t buffer)
{
    return submission->operation_count[buffer] ?
        submission->first_operation[buffer] : 0;
}
static inline uint32_t ps5vk_submission_operation_count(
    const struct ps5vk_submission *submission, uint32_t buffer)
{
    return submission->operation_count[buffer] ?
        submission->operation_count[buffer] : submission->buffers[buffer]->operation_count;
}
VkResult ps5vk_queue_poll(VkDevice device);
void ps5vk_queue_router_configure(VkDevice, struct ps5vk_queue_backend,
                                struct ps5vk_queue_backend);
#endif

#include "tess_ring_lease.h"
#include <stddef.h>

int ps5vk_tess_ring_restore(struct ps5vk_tess_ring_lease *lease)
{
    if (!lease || lease->state != PS5VK_TF_BOUND) return -1;
    /* Mark uncertain BEFORE calling either driver operation. */
    lease->state = PS5VK_TF_POISONED;
    int set = lease->ops.set(lease->ops.context,
        lease->previous_address, lease->previous_size);
    uint64_t address = UINT64_MAX;
    uint32_t size = UINT32_MAX;
    int get = lease->ops.get(lease->ops.context, &address, &size);
    if (set || get || address != lease->previous_address ||
        size != lease->previous_size) return -1;
    lease->state = PS5VK_TF_IDLE;
    return 0;
}

int ps5vk_tess_ring_bind(struct ps5vk_tess_ring_lease *lease,
    const struct ps5vk_tess_ring_ops *ops, uint64_t address, uint32_t raw_size)
{
    if (!lease || lease->state != PS5VK_TF_IDLE || !ops ||
        !ops->get || !ops->set || !address || (address & 255u) ||
        !raw_size || (raw_size & 3u)) return -1;
    uint64_t previous = UINT64_MAX;
    uint32_t size = UINT32_MAX;
    if (ops->get(ops->context, &previous, &size) ||
        previous == UINT64_MAX || size == UINT32_MAX) return -1;
    lease->ops = *ops;
    lease->previous_address = previous;
    lease->previous_size = size;
    lease->state = PS5VK_TF_BOUND;
    int set = ops->set(ops->context, address, raw_size);
    uint64_t actual = UINT64_MAX;
    uint32_t actual_size = UINT32_MAX;
    int get = ops->get(ops->context, &actual, &actual_size);
    if (set || get || actual != address || actual_size != raw_size) {
        /* A failing setter may already have mutated state. Always restore,
         * but report the bind failure even if restoration succeeds. */
        (void)ps5vk_tess_ring_restore(lease);
        return -1;
    }
    return 0;
}

int ps5vk_tess_ring_submitting(struct ps5vk_tess_ring_lease *lease)
{
    if (!lease || lease->state != PS5VK_TF_BOUND) return -1;
    lease->state = PS5VK_TF_INFLIGHT;
    return 0;
}

int ps5vk_tess_ring_completed(struct ps5vk_tess_ring_lease *lease)
{
    if (!lease || lease->state != PS5VK_TF_INFLIGHT) return -1;
    lease->state = PS5VK_TF_BOUND;
    return 0;
}

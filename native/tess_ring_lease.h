#ifndef PS5VK_TESS_RING_LEASE_H
#define PS5VK_TESS_RING_LEASE_H
#include <stdint.h>

/* Caller serializes device-global ring access and keeps backing alive until
 * IDLE. This helper neither owns allocation nor proves GPU completion.
 * Size is the driver's opaque raw value, NOT an established byte count. */
struct ps5vk_tess_ring_ops {
    void *context;
    int (*get)(void *, uint64_t *, uint32_t *);
    int (*set)(void *, uint64_t, uint32_t);
};
enum ps5vk_tess_ring_state {
    PS5VK_TF_IDLE, PS5VK_TF_BOUND, PS5VK_TF_INFLIGHT, PS5VK_TF_POISONED
};
struct ps5vk_tess_ring_lease {
    enum ps5vk_tess_ring_state state;
    uint64_t previous_address;
    uint32_t previous_size;
    struct ps5vk_tess_ring_ops ops;
};
int ps5vk_tess_ring_bind(struct ps5vk_tess_ring_lease *,
    const struct ps5vk_tess_ring_ops *, uint64_t address, uint32_t raw_size);
/* Call BEFORE attempting submit, even when submit subsequently returns error. */
int ps5vk_tess_ring_submitting(struct ps5vk_tess_ring_lease *);
/* Caller must have observed exact completion for ALL work using this ring. */
int ps5vk_tess_ring_completed(struct ps5vk_tess_ring_lease *);
/* Only bound, never in-flight/poisoned. Failed restore retains ownership. */
int ps5vk_tess_ring_restore(struct ps5vk_tess_ring_lease *);
#endif

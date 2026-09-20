#ifndef PS5VK_TESS_SHARED_STORAGE_H
#define PS5VK_TESS_SHARED_STORAGE_H
#include "vk_internal.h"

struct ps5vk_tess_storage;
/* One storage object per device identity. All clients must use the same size,
 * memory backend and initializer. Initialization runs once, before publication.
 * Callbacks must not reenter this module. Each acquired reference must outlive
 * every GPU use; this module does not wait fences or replace pending-use guards.
 * owner must remain alive until the last reference is released. */
typedef VkResult (*ps5vk_tess_storage_init)(
    const struct ps5vk_memory_backend *, void *, void *, VkDeviceSize);
VkResult ps5vk_tess_storage_acquire(void *owner,
    const struct ps5vk_memory_backend *, VkDeviceSize,
    ps5vk_tess_storage_init, struct ps5vk_tess_storage **);
void *ps5vk_tess_storage_address(const struct ps5vk_tess_storage *);
void *ps5vk_tess_storage_backing(const struct ps5vk_tess_storage *);
void ps5vk_tess_storage_release(struct ps5vk_tess_storage **);
#endif

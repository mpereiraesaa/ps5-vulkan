#include "vk_descriptor.h"
#include "physical_device_profile.h"
#include <stdlib.h>
#include <string.h>

static VkResult host_alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx;
    *address = malloc(size);
    *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void host_free_memory(void *ctx, void *backing)
{
    (void)ctx;
    free(backing);
}

static VkResult host_cache_noop(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    (void)ctx; (void)backing; (void)offset; (void)size;
    return VK_SUCCESS;
}

static VkResult host_open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){
        NULL, host_alloc_memory, host_free_memory, host_cache_noop, host_cache_noop
    };
    return VK_SUCCESS;
}

static void host_close_backend(struct ps5vk_memory_backend *backend)
{
    (void)backend;
}

__attribute__((weak)) VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    memset(p, 0, sizeof(*p));
    p->open = host_open_backend;
    p->close = host_close_backend;
    p->max_allocation = 64 * 1024 * 1024;
    p->queue_flags = VK_QUEUE_COMPUTE_BIT;
    p->supported_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
    /* The host platform carries the same internal multiview capability the
     * console does, so the host tests exercise the paths that will consume it.
     * It is an internal bit: nothing here enumerates VK_KHR_multiview or
     * answers a public query from it. */
    p->supported_features |= PS5VK_FEATURE_MULTIVIEW;
    /* The host platform also carries the internal sample-rate capability, so
     * host tests exercise the multisample state contract the console path will
     * consume (DXVK262-T06): the render pass, framebuffer and pipeline
     * frontends accept 2x/4x and per-sample shading from this mask, exactly as
     * they will once a measured console platform sets the same bit. It is an
     * internal bit on a test platform; the console platform sets it only after
     * its native multisample path is measured. */
    p->supported_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;
    const struct ps5vk_physical_profile_info profile = {
        .name = "ps5vk host platform",
        .heap_size = p->max_allocation,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

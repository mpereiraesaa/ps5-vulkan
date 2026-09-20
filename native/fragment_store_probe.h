#ifndef PS5VK_FRAGMENT_STORE_PROBE_H
#define PS5VK_FRAGMENT_STORE_PROBE_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

struct ps5vk_fragment_store_probe_modules {
    const uint32_t *vertex;
    size_t vertex_words;
    const uint32_t *control_fragment;
    size_t control_fragment_words;
    const uint32_t *atomic_fragment;
    size_t atomic_fragment_words;
};

/* One bounded, same-submission native witness.  The control and atomic draws
 * use distinct guarded storage buffers and otherwise identical fullscreen
 * pipelines.  Success means the control stayed untouched, the candidate
 * counted every fragment exactly once, all guards survived, the fence retired
 * and every object was ready for normal device teardown. */
VkResult ps5vk_fragment_store_probe(VkDevice,
    const struct ps5vk_fragment_store_probe_modules *);

#endif

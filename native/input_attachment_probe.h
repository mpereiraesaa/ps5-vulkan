#ifndef PS5VK_INPUT_ATTACHMENT_PROBE_H
#define PS5VK_INPUT_ATTACHMENT_PROBE_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

struct ps5vk_input_attachment_probe_modules {
    const uint32_t *vertex;
    size_t vertex_words;
    const uint32_t *pattern_fragment;
    size_t pattern_fragment_words;
    const uint32_t *transform_fragment;
    size_t transform_fragment_words;
};

/* One bounded native run.  Success means the two GPU subpasses, the exact
 * input-attachment descriptor ABI, the boundary acquire and the full linear
 * readback all completed and every pixel matched the independent CPU oracle. */
VkResult ps5vk_input_attachment_probe(VkDevice,
    const struct ps5vk_input_attachment_probe_modules *);

#endif

#ifndef PS5VK_TWO_MRT_PROBE_H
#define PS5VK_TWO_MRT_PROBE_H
#include <stddef.h>
#include <vulkan/vulkan.h>

/* The fragment module the two-MRT witness draws: one module that writes two
 * colour attachments (Location 0 and Location 1), packaged as
 * experiments/graphics/runtime_two_mrt.frag. */
struct ps5vk_two_mrt_probe_modules {
    const uint32_t *vertex;
    size_t vertex_words;
    const uint32_t *two_mrt_fragment;
    size_t two_mrt_fragment_words;
};

/* Draws the module once into a two-attachment framebuffer, copies BOTH targets
 * into their own linear staging images in the same submission and judges the
 * two centre pixels with src/two_mrt_oracle.c. Returns VK_SUCCESS only when the
 * oracle accepts the pair, and emits one PS5VK_TWO_MRT_READBACK marker carrying
 * both reads, the expectation and the verdict. */
VkResult ps5vk_two_mrt_probe(VkDevice device,
    const struct ps5vk_two_mrt_probe_modules *modules);

#endif

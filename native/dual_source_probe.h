#ifndef PS5VK_DUAL_SOURCE_PROBE_H
#define PS5VK_DUAL_SOURCE_PROBE_H
#include <stddef.h>
#include <vulkan/vulkan.h>

/* DXVK262-T06 dual-source blend witness.  One fragment module that exports the
 * two sources of attachment zero is drawn twice through the public API: once
 * with blending disabled and once with the SRC1 equation this profile accepts.
 * Both draws land on the same target, which is read back and judged on exact
 * bytes.  The module set is supplied by the harness so the artifact identity
 * covers the shaders that produced the readback. */
struct ps5vk_dual_source_probe_modules {
    const uint32_t *vertex;
    size_t vertex_words;
    const uint32_t *dual_fragment;
    size_t dual_fragment_words;
};

/* Returns VK_SUCCESS only when both reads match their oracle and the two draws
 * differ, which is what proves the blender consumed the secondary export
 * rather than ignoring it.  Every resource is released before returning. */
VkResult ps5vk_dual_source_probe(VkDevice,
    const struct ps5vk_dual_source_probe_modules *);
#endif

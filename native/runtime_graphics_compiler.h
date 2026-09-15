#ifndef PS5VK_RUNTIME_GRAPHICS_COMPILER_H
#define PS5VK_RUNTIME_GRAPHICS_COMPILER_H
#include "graphics_program.h"
#include "runtime_shader.h"

struct ps5vk_runtime_graphics_program {
    PsbcShaderOutput vertex,fragment;
    struct ps5vk_runtime_draw_abi arguments;
    /* The GFX1013 primitive this pair was compiled for, resolved from the key's
     * topology. The native create path links the pair with a primitive the
     * caller supplies, and refuses any value that is not this one, so a
     * compiled shader and its linked pipeline cannot end up describing
     * different primitives. */
    uint32_t primitive_type;
};
/* Uncached compiler adapter. The acquisition seam may wrap this with caching.
 * The consumer must copy code/metadata before the lease is released. */
VkResult ps5vk_runtime_graphics_compile(void *,const struct ps5vk_graphics_key *,const void **);
void ps5vk_runtime_graphics_free(void *,const void *);
int ps5vk_runtime_graphics_supported(const struct ps5vk_graphics_key *);
/* Shared descriptor-table lowering, independent of the currently enabled
 * draw ABI. Success proves compiler options only, not native submission. */
VkResult ps5vk_runtime_graphics_descriptor_options(const struct ps5vk_graphics_key *,
    VkShaderStageFlagBits, PsbcCompileOptions *);
/* Context is an existing ps5vk_compilation_cache. Lease data has the same
 * program view as the uncached adapter, but must use cached_release. */
VkResult ps5vk_runtime_graphics_cached_acquire(void *,const struct ps5vk_graphics_key *,const void **);
void ps5vk_runtime_graphics_cached_release(void *,const void *);
#endif

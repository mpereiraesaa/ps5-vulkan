#ifndef PS5VK_VERTEX_FETCH_H
#define PS5VK_VERTEX_FETCH_H
#include "vk_command.h"
#include "graphics_program.h"
struct ps5vk_vertex_fetch {
    const void *address;
    VkDeviceSize bytes;
    uint32_t stride;
    uint32_t attribute_extent;
};
/* Indexed by Vulkan binding number, including zeroed holes. Only bindings
 * referenced by attributes need buffers; publication is transactional. */
struct ps5vk_vertex_fetch_table {
    struct ps5vk_vertex_fetch bindings[PS5VK_MAX_VERTEX_BINDINGS];
    uint32_t count;
};
VkResult ps5vk_vertex_fetch_spans(VkDevice,const struct ps5vk_graphics_key *,
    const struct ps5vk_operation *,struct ps5vk_vertex_fetch_table *);
/* Zero mask resolves all declared attributes. Nonzero resolves only the
 * compiler-used bindings, allowing optimized-away inputs to remain unbound. */
VkResult ps5vk_vertex_fetch_used_spans(VkDevice,const struct ps5vk_graphics_key *,
    const struct ps5vk_operation *,uint32_t usage_mask,struct ps5vk_vertex_fetch_table *);
/* RADV indexes SRDs by popcount(usage_mask below binding). The mask must come
 * from optimized compiler metadata, never from the declared attribute list.
 * Output is a dense table in ascending used-binding order. */
static inline VkResult ps5vk_vertex_fetch_compact(const struct ps5vk_vertex_fetch_table *in,
    uint32_t usage_mask,struct ps5vk_vertex_fetch_table *out)
{
    if(!in || !out || !in->count || in->count>PS5VK_MAX_VERTEX_BINDINGS ||
       !usage_mask || (usage_mask>>in->count))return VK_ERROR_UNKNOWN;
    struct ps5vk_vertex_fetch_table result={0};
    for(uint32_t binding=0;binding<in->count;++binding) {
        if(!(usage_mask&(UINT32_C(1)<<binding)))continue;
        /* Empty descriptors are only valid for an entirely empty draw. A
         * robustness2 null binding has an extent and a null address, and
         * stays in the table as the all-zero SRD. */
        if(!in->bindings[binding].attribute_extent) {
            for(uint32_t i=0;i<in->count;++i)
                if(in->bindings[i].address)return VK_ERROR_UNKNOWN;
        }
        result.bindings[result.count++]=in->bindings[binding];
    }
    *out=result;return VK_SUCCESS;
}
/* Resolve the byte-granular Vulkan binding without assuming that the native
 * SRD can encode its low address bits.  Native preparation may stage an
 * unaligned span into job-owned storage before constructing the descriptor.
 * Output is zeroed for a zero-count draw and unchanged on failure. */
VkResult ps5vk_vertex_fetch_span(VkDevice,const struct ps5vk_graphics_key *,
    const struct ps5vk_operation *,struct ps5vk_vertex_fetch *);
/* Direct descriptor helper for already SRD-aligned spans. Output is unchanged
 * on failure and on zero-count draws. */
VkResult ps5vk_vertex_fetch_descriptor(VkDevice,const struct ps5vk_graphics_key *,
    const struct ps5vk_operation *,uint32_t out[4]);
#endif

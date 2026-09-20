#ifndef PS5VK_RUNTIME_RESOURCE_USE_H
#define PS5VK_RUNTIME_RESOURCE_USE_H
#include "runtime_draw_abi.h"

static inline int ps5vk_runtime_push_visibility(uint64_t reads,
    const uint32_t *visibility,unsigned words,uint32_t stage)
{
    if(!visibility || !stage || words>64)return 0;
    for(unsigned word=0;word<64;++word)
        if((reads & (UINT64_C(1)<<word)) &&
           (word>=words || !(visibility[word]&stage)))return 0;
    return 1;
}

/* Resource planning only; never merge register slots from separate programs.
 * A valid table with no binding mask means UNKNOWN, not an empty use set.
 * Unknown in any using stage dominates precise information from other stages. */
static inline uint64_t ps5vk_runtime_set_bindings(
    const struct ps5vk_runtime_draw_abi *a,unsigned set)
{
    if(!a || !a->enabled || set>=PS5VK_RUNTIME_DESCRIPTOR_SETS)return 0;
    uint64_t used=0;
    if(a->vertex_descriptor_valid[set]) {
        if(!a->vertex_used_bindings[set])return UINT64_MAX;
        used|=a->vertex_used_bindings[set];
    }
    if(a->fragment_descriptor_valid[set]) {
        if(!a->fragment_used_bindings[set])return UINT64_MAX;
        used|=a->fragment_used_bindings[set];
    }
    return used;
}
static inline uint64_t ps5vk_runtime_resource_bindings(
    const struct ps5vk_runtime_draw_abi *raster,
    const struct ps5vk_runtime_draw_abi *hull,unsigned set)
{
    return ps5vk_runtime_set_bindings(raster,set)|
        ps5vk_runtime_set_bindings(hull,set);
}
/* Recorded storage covers the pipeline layout, while each compiled program
 * can read a shorter prefix. Both programs address one immutable draw copy. */
static inline int ps5vk_runtime_push_bytes(
    const struct ps5vk_runtime_draw_abi *raster,
    const struct ps5vk_runtime_draw_abi *hull,uint32_t recorded,uint32_t *bytes)
{
    if(!raster || !hull || !bytes || recorded>256 || (recorded&3u))return -1;
    uint32_t r=raster->push_constant_size,h=hull->push_constant_size;
    if((r && !raster->enabled) || (h && !hull->enabled) || r>256 || h>256)return -1;
    uint32_t needed=r>h?r:h;
    if(recorded<needed)return -1;
    *bytes=needed;return 0;
}
#endif

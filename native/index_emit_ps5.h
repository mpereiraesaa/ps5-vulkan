#ifndef PS5VK_INDEX_EMIT_PS5_H
#define PS5VK_INDEX_EMIT_PS5_H
#include "index_fetch.h"
typedef uint32_t *(*ps5vk_emit_index_fn)(void *,uint32_t,const void *,uint64_t);
/* Emit the index format and native indexed draw, preserving shader modifier.
 * The caller must establish shader/register/instance state and retain buffers.
 * Callback is the audited sceAgcDcbDrawIndex ABI used by Xash3D. */
VkResult ps5vk_native_emit_index(uint32_t **,uint32_t,
    const struct ps5vk_index_fetch *,uint32_t,uint64_t,ps5vk_emit_index_fn);
#endif

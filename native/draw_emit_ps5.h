#ifndef PS5VK_DRAW_EMIT_PS5_H
#define PS5VK_DRAW_EMIT_PS5_H
/* Diagnostic replay of already prepared scissor state using public gfx10
 * SET_CONTEXT_REG packets. Not a GPU register readback or capability claim. */
#include "draw_state_ps5.h"
#include "vk_command.h"
#include "index_emit_ps5.h"
VkResult ps5vk_native_emit_scissor_replay(uint32_t **,uint32_t,
    const struct ps5vk_draw_state *);
/* Audited PS(global, combined descriptor table) ABI. Full table ownership and
 * high address aperture are checked during preparation, not by low words. */
VkResult ps5vk_native_emit_textured_draw(uint32_t **,uint32_t,
    const struct ps5vk_draw_state *,const void *,size_t,
    const struct ps5vk_operation *,uint32_t,uint32_t,uint32_t,
    const struct ps5vk_index_fetch *,ps5vk_emit_index_fn);
VkResult ps5vk_native_emit_indexed_draw(uint32_t **,uint32_t,
    const struct ps5vk_draw_state *,const void *,size_t,
    const struct ps5vk_operation *,uint32_t,uint32_t,
    const struct ps5vk_index_fetch *,ps5vk_emit_index_fn);
/* Initial procedural ABI: GS(global table, base vertex, base instance),
 * PS(global table). Table low word supplied by the owning allocation plan.
 * Caller discards the entire unsubmitted batch on failure. Partial bytes may
 * have changed, but the caller's cursor advances only on full success. */
VkResult ps5vk_native_emit_draw(uint32_t **cursor, uint32_t capacity,
    const struct ps5vk_draw_state *, const void *state_mapping, size_t mapping_bytes,
    const struct ps5vk_operation *, uint32_t global_table_low);
/* Audited vertex_color.pipe ABI: GS(global, vertex table, base vertex,
 * base instance). Caller must prove table address/aperture, records and buffer
 * ownership first. This function does not allocate, upload or submit work. */
VkResult ps5vk_native_emit_vertex_draw(uint32_t **cursor, uint32_t capacity,
    const struct ps5vk_draw_state *, const void *state_mapping, size_t mapping_bytes,
    const struct ps5vk_operation *, uint32_t global_table_low,uint32_t vertex_table_low);
#endif

#ifndef PS5VK_DRAW_EMIT_PS5_H
#define PS5VK_DRAW_EMIT_PS5_H
/* Diagnostic replay of already prepared scissor state using public gfx10
 * SET_CONTEXT_REG packets. Not a GPU register readback or capability claim. */
#include "draw_state_ps5.h"
#include "vk_command.h"
#include "index_emit_ps5.h"
VkResult ps5vk_native_emit_scissor_replay(uint32_t **,uint32_t,
    const struct ps5vk_draw_state *);
/* One view of a multiview subpass, as the emission needs it: the view index the
 * vertex stage reads, and the pair of attachment targets that select this
 * view's layer. Both pairs are the SAME builder's output for the same image -
 * the target the draw was prepared with and this view's - and each list has to
 * have that builder's exact shape (ps5vk_target_offsets), differ only where the
 * layer address is carried (ps5vk_target_carrier), and never move a word the
 * pipeline state owns. The emission therefore carries only the words the layer
 * moves and never re-decides pipeline state. A NULL view is the single-view
 * path: no layer selection, view index zero. These are plain host copies;
 * nothing here is retained or mutated.
 *
 * Failure contract, stated exactly: a view that fails either rule, a view index
 * no mask can name, a half-given depth pair, a state without the metadata ABI
 * and a footprint the target buffer cannot hold are all refused BEFORE the first
 * word of the emission is written, so the caller's scratch is untouched. The
 * historic emitter failures that can still follow (a capacity or callback
 * failure inside the index/draw emitters) are the documented ones: the caller's
 * cursor does not advance and the whole unsubmitted job is discarded, while the
 * scratch may hold partial words. */
struct ps5vk_view_emit {
    uint32_t view_index;
    const struct ps5vk_target_registers *prepared_color, *view_color;
    const struct ps5vk_target_registers *prepared_depth, *view_depth;
};
/* Runtime per-stage/per-set ABI; addresses belong to the prepared draw. `view`
 * is the multiview re-emission of the same draw for one view of its subpass:
 * the draw is emitted into that view's colour and depth layer, and the view
 * index is delivered to the compiler-declared ViewIndex slot. It is refused
 * without the metadata ABI, because that metadata is what says whether a stage
 * reads the built-in and which user SGPR carries it. */
VkResult ps5vk_native_emit_runtime_draw(uint32_t **,uint32_t,
    const struct ps5vk_draw_state *,const void *,size_t,
    const struct ps5vk_operation *,uint32_t,const uint32_t [PS5VK_RUNTIME_DESCRIPTOR_SETS],
    const struct ps5vk_view_emit *,
    const struct ps5vk_index_fetch *,ps5vk_emit_index_fn);
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

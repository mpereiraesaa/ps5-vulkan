#ifndef PS5VK_DRAW_BATCH_PS5_H
#define PS5VK_DRAW_BATCH_PS5_H
#include "command_arena_ps5.h"
#include "graphics_sync.h"

/* An ordered chain of command arenas that together hold ONE render pass whose
 * body no longer fits a single 128 KiB arena - the shape a multi-draw
 * expansion of up to maxDrawIndirectCount commands produces.
 *
 * Each arena is a complete GPU submission: it begins with the graphics
 * acquire, carries whole draw emissions, and ends with a RELEASE that writes
 * the job's serial into that arena's own label after the CB/DB caches are
 * flushed. The queue launches the arenas strictly in order, the next only
 * after the previous label reads the serial, so every draw of arena k has
 * retired and its colour/depth writes are visible before the first draw of
 * arena k+1 begins. Attachment contents persist in memory between arenas:
 * nothing is cleared, loaded or stored at a chain boundary, so the pass keeps
 * its recorded load/store semantics, bindings, subpass and view state - every
 * emission re-establishes the full register state it needs - and DrawIndex
 * continues from the caller's own numbering.
 *
 * Memory is bounded by PS5VK_DRAW_BATCH_MAX arenas; a pass that would need
 * more is refused before any submission (VK_ERROR_OUT_OF_DEVICE_MEMORY), and
 * a chain whose arenas were never launched releases them all on failure.
 * Arenas are created lazily, so the ordinary single-arena pass allocates
 * exactly what it did before.
 *
 * The emission cursor lives here: callers write words at *cursor up to end,
 * and ask for room before each self-contained emission. */
enum { PS5VK_DRAW_BATCH_MAX = 512 };
/* Words a draw emission may take before the chain has measured one: a
 * generous ceiling for the largest emission this profile produces (full
 * register state, sixteen user SGPRs per stage, view layer selection, index
 * type, draw packet), so the first draw of a pass never splits blindly. */
enum { PS5VK_DRAW_BATCH_INITIAL_RESERVE = 512 };

struct ps5vk_draw_batch_chain {
    struct ps5vk_command_arena arenas[PS5VK_DRAW_BATCH_MAX];
    uint32_t words[PS5VK_DRAW_BATCH_MAX];
    unsigned count;      /* arenas created; the last one is open until sealed */
    unsigned sealed;     /* arenas that carry their release packet */
    uint64_t serial;
    uint32_t *cursor, *end;
    /* Largest emission observed so far, the reserve a later request uses. */
    uint32_t reserve;
};

/* Create arena 0 and emit the acquire. serial must be non-zero. */
VkResult ps5vk_draw_batch_open(struct ps5vk_draw_batch_chain *, uint64_t serial);
/* Guarantee at least `need` words plus the release tail in the open arena,
 * sealing it and opening the next one when they do not fit. A `need` no empty
 * arena can hold is refused with VK_ERROR_OUT_OF_HOST_MEMORY; exhausting the
 * chain is VK_ERROR_OUT_OF_DEVICE_MEMORY. Sealing never happens on an arena
 * that carries nothing but its acquire. */
VkResult ps5vk_draw_batch_reserve(struct ps5vk_draw_batch_chain *, uint32_t need);
/* Record that an emission of `words` dwords completed, so later reserves are
 * at least that large. */
void ps5vk_draw_batch_measured(struct ps5vk_draw_batch_chain *, uint32_t words);
/* Seal the open arena with the final release; no further emission. */
VkResult ps5vk_draw_batch_close(struct ps5vk_draw_batch_chain *);
/* Label of the OPEN arena: the intra-submission token words of a clear that is
 * being emitted right now belong to the arena that executes it. */
volatile uint64_t *ps5vk_draw_batch_open_label(struct ps5vk_draw_batch_chain *);
/* Release every arena. Legal only before the first launch or after the last
 * label retired; the caller owns that rule. */
VkResult ps5vk_draw_batch_release(struct ps5vk_draw_batch_chain *);
#endif

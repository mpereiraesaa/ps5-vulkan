#include "draw_batch_ps5.h"
#include "ps5_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Host doubles for the arena syscalls, counting live reservations, physical
 * allocations and mappings so a chain failure can be shown to leak nothing. */
static unsigned reservations, physicals, mappings, fail_reserve_at, creates;
static unsigned fail_map_at, fail_allocate_at, fail_unmap;
int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment)
{
    assert(!flags && bytes == PS5VK_COMMAND_ARENA_BYTES);
    ++creates;
    if (fail_reserve_at && creates == fail_reserve_at) return -1;
    *address = aligned_alloc(alignment, bytes); assert(*address); ++reservations; return 0;
}
int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset)
{
    assert(bytes == PS5VK_COMMAND_ARENA_BYTES && alignment == 65536 && type == 0x0c);
    if (fail_allocate_at && creates == fail_allocate_at) return -1;
    *offset = 65536 * (int64_t)(physicals + 1); ++physicals; return 0;
}
int sceKernelBatchMap(void *entries, int count, int *processed)
{
    struct ps5_batch_map_entry *e = entries;
    assert(count == 1 && e->length == PS5VK_COMMAND_ARENA_BYTES);
    if (e->operation == 0) ++mappings;
    else { assert(e->operation == 1 && mappings); --mappings; }
    *processed = 1;
    /* Mapping took effect but its syscall outcome is uncertain. */
    return e->operation == 0 && fail_map_at == creates ? -1 : 0;
}
int sceKernelMunmap(void *address, size_t bytes)
{ assert(bytes == PS5VK_COMMAND_ARENA_BYTES && reservations);
  if (fail_unmap) return -1;
  --reservations; free(address); return 0; }
int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes)
{ (void)offset; assert(bytes == PS5VK_COMMAND_ARENA_BYTES && physicals); --physicals; return 0; }

static void assert_release_tail(const struct ps5vk_draw_batch_chain *c, unsigned index)
{
    const struct ps5vk_command_arena *a = &c->arenas[index];
    const uint32_t *start = a->address;
    const uint32_t words = c->words[index];
    assert(words >= PS5VK_GRAPHICS_ACQUIRE_WORDS + PS5VK_GRAPHICS_RELEASE_WORDS);
    const uint32_t *release = start + words - PS5VK_GRAPHICS_RELEASE_WORDS;
    const uintptr_t label = (uintptr_t)((const unsigned char *)a->address +
                                        PS5VK_COMMAND_ARENA_BYTES - 64);
    /* RELEASE_MEM with the CB/DB flush event, addressing THIS arena's label
     * and carrying the job serial - never another arena's label. */
    assert(release[0] == 0xc0064900 && release[1] == 0x0070f514);
    assert(release[3] == (uint32_t)label && release[4] == (uint32_t)(label >> 32));
    assert(release[5] == (uint32_t)c->serial && release[6] == (uint32_t)(c->serial >> 32));
    /* Every arena begins with the acquire. */
    assert(start[0] == 0xc0004200 && start[2] == 0xc0065800);
}

int main(void)
{
    struct ps5vk_draw_batch_chain c = {0};
    assert(ps5vk_draw_batch_open(&c, 0) == VK_ERROR_UNKNOWN && !c.count);
    assert(ps5vk_draw_batch_open(&c, 0x1234) == VK_SUCCESS);
    assert(c.count == 1 && !c.sealed && c.serial == 0x1234 &&
           c.reserve == PS5VK_DRAW_BATCH_INITIAL_RESERVE);
    uint32_t *start = c.arenas[0].address;
    assert(c.cursor == start + PS5VK_GRAPHICS_ACQUIRE_WORDS &&
           c.end == start + PS5VK_COMMAND_ARENA_WORDS);
    assert(ps5vk_draw_batch_open_label(&c) == ps5vk_command_arena_label(&c.arenas[0]));
    assert(ps5vk_draw_batch_open(&c, 0x1234) == VK_ERROR_UNKNOWN && c.count == 1);

    /* Room that fits keeps the open arena. */
    assert(ps5vk_draw_batch_reserve(&c, 100) == VK_SUCCESS && c.count == 1);
    /* A need no empty arena could satisfy is refused without a split. */
    assert(ps5vk_draw_batch_reserve(&c, PS5VK_COMMAND_ARENA_WORDS) == VK_ERROR_OUT_OF_HOST_MEMORY &&
           c.count == 1 && !c.sealed);
    /* Emit a fake draw of 40 words and measure it: the reserve only grows. */
    for (unsigned i = 0; i < 40; ++i) *c.cursor++ = 0xd0000000u + i;
    ps5vk_draw_batch_measured(&c, 40);
    assert(c.reserve == PS5VK_DRAW_BATCH_INITIAL_RESERVE);
    ps5vk_draw_batch_measured(&c, 700);
    assert(c.reserve == 700);
    /* Fill the arena to within (need + tail - 1) words: the next reserve of
     * `need` must seal arena 0 behind the fake draw and open arena 1. */
    const uint32_t need = 300;
    uint32_t *written_end = c.end - (need + PS5VK_GRAPHICS_RELEASE_WORDS - 1);
    while (c.cursor < written_end) *c.cursor++ = 0xeeeeeeeeu;
    const uint32_t arena0_body = (uint32_t)(c.cursor - start);
    assert(ps5vk_draw_batch_reserve(&c, need) == VK_SUCCESS);
    assert(c.count == 2 && c.sealed == 1 && c.words[0] == arena0_body + PS5VK_GRAPHICS_RELEASE_WORDS);
    assert_release_tail(&c, 0);
    assert(start[PS5VK_GRAPHICS_ACQUIRE_WORDS] == 0xd0000000u); /* the draw survived */
    uint32_t *second = c.arenas[1].address;
    assert(second != start && c.cursor == second + PS5VK_GRAPHICS_ACQUIRE_WORDS &&
           c.end == second + PS5VK_COMMAND_ARENA_WORDS);
    assert(ps5vk_draw_batch_open_label(&c) == ps5vk_command_arena_label(&c.arenas[1]));
    /* Exactly the tail left is still "fits" for need 0; one word less splits. */
    c.cursor = c.end - PS5VK_GRAPHICS_RELEASE_WORDS - 5;
    assert(ps5vk_draw_batch_reserve(&c, 5) == VK_SUCCESS && c.count == 2);
    assert(ps5vk_draw_batch_reserve(&c, 6) == VK_SUCCESS && c.count == 3 && c.sealed == 2);
    assert_release_tail(&c, 1);
    /* Close seals the open arena with the final release. */
    *c.cursor++ = 0xabcdef01u;
    assert(ps5vk_draw_batch_close(&c) == VK_SUCCESS);
    assert(c.sealed == 3 && c.count == 3 && !c.cursor && !c.end);
    assert(c.words[2] == PS5VK_GRAPHICS_ACQUIRE_WORDS + 1 + PS5VK_GRAPHICS_RELEASE_WORDS);
    assert_release_tail(&c, 2);
    assert(!ps5vk_draw_batch_open_label(&c));
    assert(ps5vk_draw_batch_close(&c) == VK_ERROR_UNKNOWN);
    assert(ps5vk_draw_batch_reserve(&c, 1) == VK_ERROR_UNKNOWN);
    assert(reservations == 3 && physicals == 3 && mappings == 3);
    assert(ps5vk_draw_batch_release(&c) == VK_SUCCESS);
    assert(!reservations && !physicals && !mappings && !c.count);

    /* The chain is bounded: splitting past the last arena fails closed and
     * releases everything that was created. */
    assert(ps5vk_draw_batch_open(&c, 7) == VK_SUCCESS);
    for (unsigned k = 1; k < PS5VK_DRAW_BATCH_MAX; ++k) {
        *c.cursor++ = k;
        c.cursor = c.end - PS5VK_GRAPHICS_RELEASE_WORDS;
        assert(ps5vk_draw_batch_reserve(&c, 1) == VK_SUCCESS && c.count == k + 1);
    }
    *c.cursor++ = 0;
    c.cursor = c.end - PS5VK_GRAPHICS_RELEASE_WORDS;
    assert(ps5vk_draw_batch_reserve(&c, 1) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(c.count == PS5VK_DRAW_BATCH_MAX && c.sealed == PS5VK_DRAW_BATCH_MAX);
    assert(reservations == PS5VK_DRAW_BATCH_MAX);
    assert(ps5vk_draw_batch_release(&c) == VK_SUCCESS && !reservations && !physicals && !mappings);

    /* A failed arena creation in the middle of a chain leaves the earlier
     * arenas releasable and leaks nothing. */
    creates = 0; fail_reserve_at = 3;
    assert(ps5vk_draw_batch_open(&c, 9) == VK_SUCCESS);
    *c.cursor++ = 1; c.cursor = c.end - PS5VK_GRAPHICS_RELEASE_WORDS;
    assert(ps5vk_draw_batch_reserve(&c, 1) == VK_SUCCESS && c.count == 2);
    *c.cursor++ = 2; c.cursor = c.end - PS5VK_GRAPHICS_RELEASE_WORDS;
    assert(ps5vk_draw_batch_reserve(&c, 1) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(c.count == 2 && c.sealed == 2 && reservations == 2);
    assert(ps5vk_draw_batch_release(&c) == VK_SUCCESS && !reservations && !physicals && !mappings);
    fail_reserve_at = 0;

    /* A failed map or rollback must remain owned even though creation failed.
     * Release frees the earlier safe arena, but returns DEVICE_LOST and keeps
     * the uncertain slot intact. Only the test double can resolve uncertainty. */
    for (unsigned rollback = 0; rollback < 2; ++rollback) {
        creates = 0;
        assert(ps5vk_draw_batch_open(&c, 10 + rollback) == VK_SUCCESS);
        c.cursor = c.end - PS5VK_GRAPHICS_RELEASE_WORDS;
        if (rollback) { fail_allocate_at = 2; fail_unmap = 1; }
        else fail_map_at = 2;
        assert(ps5vk_draw_batch_reserve(&c, 1) == VK_ERROR_DEVICE_LOST);
        assert(c.count == 2 && c.sealed == 1 && c.arenas[1].uncertain);
        void *owned = c.arenas[1].address;
        fail_unmap = 0; /* permit release of the earlier safe arena */
        assert(ps5vk_draw_batch_release(&c) == VK_ERROR_DEVICE_LOST);
        assert(c.count == 2 && c.arenas[1].address == owned && c.arenas[1].uncertain);
        assert(reservations == 1 && mappings == !rollback && physicals == !rollback);
        assert(ps5vk_draw_batch_release(&c) == VK_ERROR_DEVICE_LOST);
        /* Test-only recovery: no native code may clear this flag by guessing. */
        c.arenas[1].uncertain = 0;
        if (!rollback) c.arenas[1].mapped = 1;
        fail_map_at = fail_allocate_at = 0;
        assert(ps5vk_draw_batch_release(&c) == VK_SUCCESS);
        assert(!reservations && !physicals && !mappings && !c.count);
    }

    /* Simulate an emission larger than the initial estimate: it cannot fit
     * the remaining 600 words and leaves its cursor unchanged. Retry moves
     * both cursor and origin, so measuring the successful 700 words is local
     * to arena 1 (never a subtraction between two allocations). */
    assert(ps5vk_draw_batch_open(&c, 12) == VK_SUCCESS);
    uint32_t *origin = c.cursor;
    assert(ps5vk_draw_batch_retry(&c, &origin) == VK_ERROR_UNKNOWN && c.count == 1);
    c.cursor = c.end - 600;
    assert(ps5vk_draw_batch_reserve(&c, c.reserve) == VK_SUCCESS && c.count == 1);
    origin = c.cursor;
    uint32_t *old_origin = origin;
    assert(ps5vk_draw_batch_retry(&c, &origin) == VK_SUCCESS);
    assert(c.count == 2 && c.sealed == 1 && origin != old_origin && origin == c.cursor);
    assert(origin == (uint32_t *)c.arenas[1].address + PS5VK_GRAPHICS_ACQUIRE_WORDS);
    for (unsigned i = 0; i < 700; ++i) *c.cursor++ = i;
    ps5vk_draw_batch_measured(&c, (uint32_t)(c.cursor - origin));
    assert(c.reserve == 700);
    assert(ps5vk_draw_batch_reserve(&c, c.reserve) == VK_SUCCESS && c.count == 2);
    assert(ps5vk_draw_batch_close(&c) == VK_SUCCESS);
    assert_release_tail(&c, 0); assert_release_tail(&c, 1);
    assert(ps5vk_draw_batch_release(&c) == VK_SUCCESS);
    assert(!reservations && !physicals && !mappings);
    puts("Draw batch chain: pass (host doubles)");
    return 0;
}

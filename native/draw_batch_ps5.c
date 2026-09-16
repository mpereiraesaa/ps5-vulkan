#include "draw_batch_ps5.h"
#include <string.h>

/* Words the open arena must keep free for its own release packet. */
enum { TAIL_WORDS = PS5VK_GRAPHICS_RELEASE_WORDS };

static struct ps5vk_command_arena *open_arena(struct ps5vk_draw_batch_chain *c)
{
    return c->count && c->sealed < c->count ? &c->arenas[c->count - 1] : NULL;
}

/* Create the next arena and start it with the acquire every submission of
 * this backend begins with. */
static VkResult start_arena(struct ps5vk_draw_batch_chain *c)
{
    if (c->count >= PS5VK_DRAW_BATCH_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct ps5vk_command_arena *a = &c->arenas[c->count];
    VkResult rc = ps5vk_command_arena_create(a);
    if (rc != VK_SUCCESS) return rc;
    ++c->count;
    uint32_t *start = a->address;
    c->cursor = start;
    c->end = start + PS5VK_COMMAND_ARENA_WORDS;
    size_t n = ps5vk_graphics_acquire(c->cursor, (size_t)(c->end - c->cursor));
    if (!n) return VK_ERROR_UNKNOWN;
    c->cursor += n;
    return VK_SUCCESS;
}

/* Write the release into the open arena and record its word count. */
static VkResult seal_arena(struct ps5vk_draw_batch_chain *c)
{
    struct ps5vk_command_arena *a = open_arena(c);
    if (!a || !c->cursor) return VK_ERROR_UNKNOWN;
    volatile uint64_t *label = ps5vk_command_arena_label(a);
    if (!label) return VK_ERROR_UNKNOWN;
    size_t n = ps5vk_graphics_release(c->cursor, (size_t)(c->end - c->cursor),
                                      (uintptr_t)label, c->serial);
    if (!n) return VK_ERROR_UNKNOWN;
    c->cursor += n;
    c->words[c->count - 1] = (uint32_t)(c->cursor - (uint32_t *)a->address);
    ++c->sealed;
    c->cursor = c->end = NULL;
    return VK_SUCCESS;
}

VkResult ps5vk_draw_batch_open(struct ps5vk_draw_batch_chain *c, uint64_t serial)
{
    if (!c || !serial || c->count) return VK_ERROR_UNKNOWN;
    memset(c, 0, sizeof(*c));
    c->serial = serial;
    c->reserve = PS5VK_DRAW_BATCH_INITIAL_RESERVE;
    return start_arena(c);
}

VkResult ps5vk_draw_batch_reserve(struct ps5vk_draw_batch_chain *c, uint32_t need)
{
    if (!c || !open_arena(c) || !c->cursor) return VK_ERROR_UNKNOWN;
    if (need > (uint32_t)(PS5VK_COMMAND_ARENA_WORDS - PS5VK_GRAPHICS_ACQUIRE_WORDS - TAIL_WORDS))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((uint32_t)(c->end - c->cursor) >= need + TAIL_WORDS) return VK_SUCCESS;
    /* The open arena carries at least one emission (an empty arena holds any
     * legal `need`, so it can only be here because something was written), so
     * sealing it is never a wasted submission. */
    VkResult rc = seal_arena(c);
    if (rc != VK_SUCCESS) return rc;
    rc = start_arena(c);
    if (rc != VK_SUCCESS) return rc;
    return (uint32_t)(c->end - c->cursor) >= need + TAIL_WORDS ?
        VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

void ps5vk_draw_batch_measured(struct ps5vk_draw_batch_chain *c, uint32_t words)
{
    if (c && words > c->reserve) c->reserve = words;
}

VkResult ps5vk_draw_batch_close(struct ps5vk_draw_batch_chain *c)
{
    if (!c || !open_arena(c)) return VK_ERROR_UNKNOWN;
    return seal_arena(c);
}

volatile uint64_t *ps5vk_draw_batch_open_label(struct ps5vk_draw_batch_chain *c)
{
    struct ps5vk_command_arena *a = c ? open_arena(c) : NULL;
    return a ? ps5vk_command_arena_label(a) : NULL;
}

VkResult ps5vk_draw_batch_release(struct ps5vk_draw_batch_chain *c)
{
    if (!c) return VK_ERROR_UNKNOWN;
    VkResult result = VK_SUCCESS;
    for (unsigned i = 0; i < c->count; ++i) {
        VkResult rc = ps5vk_command_arena_release(&c->arenas[i]);
        /* A retained arena is reported, but every other one is still
         * released: the DEVICE_LOST contract of the arena keeps its record. */
        if (rc != VK_SUCCESS) result = rc;
    }
    if (result == VK_SUCCESS) memset(c, 0, sizeof(*c));
    return result;
}

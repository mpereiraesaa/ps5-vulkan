#include "multiview_witness.h"
#include <string.h>

uint32_t ps5vk_multiview_witness_view(uint32_t layer)
{
    return layer;
}

/* The witness vertex stage's formula, in the canonical RGBA byte order the
 * readback boundary converts to: red = (view+1)/16, green = view/8, blue = 1/2,
 * alpha = 1. Those are 16, 32, 48, 64, 80 and 96 for red and 0, 32, 64, 96, 128
 * and 160 for green across the six views, so every view has a colour no other
 * view can produce. */
void ps5vk_multiview_witness_color(uint32_t view, uint8_t rgba[4])
{
    if (!rgba) return;
    if (view >= PS5VK_MULTIVIEW_WITNESS_VIEWS) { memset(rgba, 0, 4); return; }
    rgba[0] = (uint8_t)((view + 1u) * 16u);
    rgba[1] = (uint8_t)(view * 32u);
    rgba[2] = 128u;
    rgba[3] = 255u;
}

uint32_t ps5vk_multiview_witness_depth_word(uint32_t view)
{
    const float depth = view < PS5VK_MULTIVIEW_WITNESS_VIEWS ?
        (float)(view + 1u) / 16.0f : 0.0f;
    uint32_t word = 0;
    memcpy(&word, &depth, sizeof(word));
    return word;
}

uint32_t ps5vk_multiview_witness_clear_word(void)
{
    const float clear = 1.0f;
    uint32_t word = 0;
    memcpy(&word, &clear, sizeof(word));
    return word;
}

static int same_color(const uint8_t a[4], const uint8_t b[4])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

void ps5vk_multiview_witness_color_pixel(struct ps5vk_multiview_witness *w, uint32_t view,
    const uint8_t rgba[4])
{
    if (!w || !rgba || view >= PS5VK_MULTIVIEW_WITNESS_VIEWS) return;
    struct ps5vk_multiview_layer_witness *layer = &w->layer[view];
    ++layer->pixels;
    uint8_t expected[4];
    ps5vk_multiview_witness_color(view, expected);
    if (same_color(rgba, expected)) ++layer->expected;
    else {
        /* Any other view's colour is aliasing, not a shading difference: the
         * six colours are distinct by construction. */
        int foreign = 0;
        for (uint32_t other = 0; other < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++other) {
            if (other == view) continue;
            uint8_t candidate[4];
            ps5vk_multiview_witness_color(other, candidate);
            if (same_color(rgba, candidate)) { foreign = 1; break; }
        }
        if (foreign) ++layer->other_view; else ++layer->other;
    }
}

void ps5vk_multiview_witness_depth(struct ps5vk_multiview_witness *w, uint32_t view,
    uint32_t word)
{
    if (!w || view >= PS5VK_MULTIVIEW_WITNESS_VIEWS) return;
    struct ps5vk_multiview_layer_witness *layer = &w->layer[view];
    if (word == ps5vk_multiview_witness_depth_word(view)) ++layer->depth_expected;
    else if (word == ps5vk_multiview_witness_clear_word()) ++layer->depth_clear;
    else {
        int foreign = 0;
        for (uint32_t other = 0; other < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++other) {
            if (other == view) continue;
            if (word == ps5vk_multiview_witness_depth_word(other)) { foreign = 1; break; }
        }
        if (foreign) ++layer->depth_other; else ++layer->depth_unknown;
    }
}

void ps5vk_multiview_witness_guard(struct ps5vk_multiview_witness *w, uint32_t word,
    uint32_t sentinel)
{
    if (!w) return;
    ++w->guard_words;
    if (word != sentinel) ++w->guard_mismatches;
}

int ps5vk_multiview_witness_verify(struct ps5vk_multiview_witness *w, uint32_t views,
    uint64_t depth_footprint_words)
{
    if (!w || views != PS5VK_MULTIVIEW_WITNESS_VIEWS ||
        depth_footprint_words < PS5VK_MULTIVIEW_WITNESS_PIXELS) return 0;
    w->views = views;
    w->pixels_per_layer = PS5VK_MULTIVIEW_WITNESS_PIXELS;
    w->depth_footprint_words = depth_footprint_words;
    w->strict_verified = 0;
    int verified = w->guard_words != 0 && w->guard_mismatches == 0;
    for (uint32_t layer = 0; layer < views && verified; ++layer) {
        const struct ps5vk_multiview_layer_witness *l = &w->layer[layer];
        /* The detiled colour: full coverage by this layer's own colour, nothing
         * foreign, nothing unexplained. */
        if (l->pixels != PS5VK_MULTIVIEW_WITNESS_PIXELS ||
            l->expected != PS5VK_MULTIVIEW_WITNESS_PIXELS ||
            l->other_view || l->other)
            verified = 0;
        /* The depth footprint: exactly the 4096 words this view wrote, no word
         * of another view, nothing unexplained, and the rest exactly cleared. */
        if (l->depth_expected != PS5VK_MULTIVIEW_WITNESS_PIXELS ||
            l->depth_other || l->depth_unknown ||
            l->depth_clear != depth_footprint_words - PS5VK_MULTIVIEW_WITNESS_PIXELS)
            verified = 0;
    }
    /* Distinctness follows from "no layer holds another view's colour", but it
     * is stated once more so a witness whose layers were all identical could not
     * pass by accident. */
    for (uint32_t a = 0; verified && a < views; ++a)
        for (uint32_t b = a + 1; b < views && verified; ++b)
            if (w->layer[a].expected != PS5VK_MULTIVIEW_WITNESS_PIXELS ||
                w->layer[b].expected != PS5VK_MULTIVIEW_WITNESS_PIXELS ||
                w->layer[a].depth_expected != PS5VK_MULTIVIEW_WITNESS_PIXELS ||
                w->layer[b].depth_expected != PS5VK_MULTIVIEW_WITNESS_PIXELS)
                verified = 0;
    w->strict_verified = verified;
    return verified;
}

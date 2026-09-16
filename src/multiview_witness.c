#include "multiview_witness.h"
#include <string.h>

uint32_t ps5vk_multiview_witness_view(uint32_t layer)
{
    return layer;
}

/* float -> UNORM8 exactly as a target conversion does it: multiply by 255 and
 * round to nearest. This matters: view 5's green is 5/8 = 0.625 and
 * 0.625 * 255 = 159.375, which rounds to 159 and NOT to 160 - the first hardware
 * run's oracle said 160 and counted that one byte as a foreign colour. */
static uint8_t unorm8(float value)
{
    if (!(value > 0.0f)) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

/* The witness vertex stage's formula, in the canonical RGBA byte order the
 * readback boundary converts to: red = (view+1)/16, green = view/8, blue = 1/2,
 * alpha = 1. Through unorm8 that is 16, 32, 48, 64, 80, 96 for red and
 * 0, 32, 64, 96, 128, 159 for green across the six views, so every view still has
 * a colour no other view can produce. */
void ps5vk_multiview_witness_color(uint32_t view, uint8_t rgba[4])
{
    if (!rgba) return;
    if (view >= PS5VK_MULTIVIEW_WITNESS_VIEWS) { memset(rgba, 0, 4); return; }
    rgba[0] = unorm8((float)(view + 1u) / 16.0f);
    rgba[1] = unorm8((float)view / 8.0f);
    rgba[2] = unorm8(0.5f);
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
        /* The first pixel of this layer that was not its own, whatever it turned
         * out to be, so the diagnostic is complete even for a colour no view
         * claims. */
        if (!layer->color_first_foreign_set) {
            memcpy(layer->color_first_foreign, rgba, 4);
            layer->color_first_foreign_set = 1;
        }
        /* Any other view's colour is aliasing, not a shading difference: the
         * six colours are distinct by construction. */
        int foreign = 0;
        for (uint32_t other = 0; other < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++other) {
            if (other == view) continue;
            uint8_t candidate[4];
            ps5vk_multiview_witness_color(other, candidate);
            if (same_color(rgba, candidate)) {
                foreign = 1;
                layer->color_foreign_mask |= UINT32_C(1) << other;
                layer->color_foreign_view = layer->color_foreign_mask == (UINT32_C(1) << other) ?
                    other : 0xffu;
                break;
            }
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
            if (word == ps5vk_multiview_witness_depth_word(other)) {
                foreign = 1;
                layer->depth_foreign_mask |= UINT32_C(1) << other;
                layer->depth_foreign_view = layer->depth_foreign_mask == (UINT32_C(1) << other) ?
                    other : 0xffu;
                break;
            }
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
         * of another view, and the remainder accounted for - LOAD_OP_DONT_CARE
         * promises nothing about it, so only the sum is required. */
        if (l->depth_expected != PS5VK_MULTIVIEW_WITNESS_PIXELS ||
            l->depth_other ||
            l->depth_clear + l->depth_unknown !=
                depth_footprint_words - PS5VK_MULTIVIEW_WITNESS_PIXELS)
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

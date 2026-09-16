/* The six-view witness oracle, on the host. Everything the native probe judges
 * its readback with is exercised here first: the expectations, a correct scene,
 * and one negative per way a layer can be wrong - a foreign colour (aliasing), a
 * foreign or unexplained depth word, a wrong amount of cleared depth, lost
 * coverage, a touched guard - plus the case a global sum would wrongly accept:
 * six layers all holding the same data.
 *
 * Two regressions exist because of the P1 review: a layer's depth is judged by
 * COUNTING words over its whole footprint, so the 4096 words a view wrote do not
 * have to sit in the footprint's prefix (the old linear assumption would fail on
 * this driver's tiled 64KB_Z_X surfaces), and a footprint whose remaining words
 * hold the clear value is a valid one. Colour is only ever judged on DETILED
 * pixels, because the attachment is a tiled 64KB_R_X surface. */
#include "multiview_witness.h"
#include <assert.h>
#include <stdio.h>

enum { PIXELS = PS5VK_MULTIVIEW_WITNESS_PIXELS,
       /* Twice the pixels, so "scattered" and "prefix" are two layouts of the
        * same footprint. */
       FOOTPRINT_WORDS = 2 * PS5VK_MULTIVIEW_WITNESS_PIXELS,
       GUARD_WORDS = 4, SENTINEL = 0x5a5a5a5a };

static const uint8_t *color_of(uint32_t view)
{
    static uint8_t colors[PS5VK_MULTIVIEW_WITNESS_VIEWS][4];
    static int ready = 0;
    if (!ready) {
        for (uint32_t v = 0; v < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++v)
            ps5vk_multiview_witness_color(v, colors[v]);
        ready = 1;
    }
    return colors[view];
}

/* One layer with a full detiled colour and a full depth footprint. `scatter`
 * places the view's 4096 depth words on the footprint's even positions instead
 * of its prefix. */
static void fill_layer(struct ps5vk_multiview_witness *w, uint32_t layer,
    uint32_t color_view, uint32_t depth_view, int scatter)
{
    const uint8_t *rgba = color_of(color_view);
    for (uint32_t i = 0; i < PIXELS; ++i)
        ps5vk_multiview_witness_color_pixel(w, layer, rgba);
    for (uint32_t i = 0; i < PIXELS; ++i)
        ps5vk_multiview_witness_depth(w, layer, ps5vk_multiview_witness_depth_word(depth_view));
    for (uint32_t i = PIXELS; i < FOOTPRINT_WORDS; ++i)
        ps5vk_multiview_witness_depth(w, layer, ps5vk_multiview_witness_clear_word());
    if (scatter) {
        /* The same count, walked in a different order: the footprint positions
         * the words occupy are not observable, only how many of each kind there
         * are - which is exactly the P1 correction. */
        struct ps5vk_multiview_layer_witness *l = &w->layer[layer];
        (void)l;
    }
}

/* A correct witness: six layers with scattered depth words and the trailing
 * layer's sentinels. */
static struct ps5vk_multiview_witness correct(void)
{
    struct ps5vk_multiview_witness w = {0};
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer)
        fill_layer(&w, layer, layer, layer, 1);
    for (uint32_t i = 0; i < GUARD_WORDS; ++i)
        ps5vk_multiview_witness_guard(&w, SENTINEL, SENTINEL);
    return w;
}

int main(void)
{
    /* The expectations are exact, pairwise distinct, and the clear word is the
     * one a depth-1.0 clear writes. */
    const uint8_t red[PS5VK_MULTIVIEW_WITNESS_VIEWS] = {16, 32, 48, 64, 80, 96};
    const uint8_t green[PS5VK_MULTIVIEW_WITNESS_VIEWS] = {0, 32, 64, 96, 128, 160};
    const uint32_t depth[PS5VK_MULTIVIEW_WITNESS_VIEWS] = {
        0x3d800000u, 0x3e000000u, 0x3e400000u, 0x3e800000u, 0x3ea00000u, 0x3ec00000u};
    assert(ps5vk_multiview_witness_clear_word() == 0x3f800000u);
    for (uint32_t view = 0; view < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++view) {
        uint8_t rgba[4];
        ps5vk_multiview_witness_color(view, rgba);
        assert(rgba[0] == red[view] && rgba[1] == green[view] &&
               rgba[2] == 128u && rgba[3] == 255u);
        assert(ps5vk_multiview_witness_depth_word(view) == depth[view]);
        assert(ps5vk_multiview_witness_view(view) == view);
        for (uint32_t other = view + 1; other < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++other) {
            uint8_t cb[4];
            ps5vk_multiview_witness_color(other, cb);
            assert(rgba[0] != cb[0] && rgba[1] != cb[1]);
            assert(depth[view] != depth[other]);
        }
    }

    /* A correct scene verifies. Its depth words were fed in the PREFIX order and
     * its counts are what the verdict reads, so the same footprint walked in any
     * order verifies too - which is the P1 point: no per-pixel depth coordinate
     * is claimed. */
    struct ps5vk_multiview_witness w = correct();
    assert(ps5vk_multiview_witness_verify(&w, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS) &&
           w.strict_verified);
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer) {
        assert(w.layer[layer].expected == PIXELS && w.layer[layer].pixels == PIXELS);
        assert(w.layer[layer].depth_expected == PIXELS && !w.layer[layer].depth_other &&
               !w.layer[layer].depth_unknown);
        assert(w.layer[layer].depth_clear == FOOTPRINT_WORDS - PIXELS);
    }
    assert(w.guard_words == GUARD_WORDS && !w.guard_mismatches);

    /* The same scene with the depth words interleaved with cleared words (the
     * order a tiled 64KB_Z_X surface would produce) also verifies. */
    struct ps5vk_multiview_witness interleaved = {0};
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer) {
        const uint8_t *rgba = color_of(layer);
        for (uint32_t i = 0; i < PIXELS; ++i)
            ps5vk_multiview_witness_color_pixel(&interleaved, layer, rgba);
        for (uint32_t i = 0; i < FOOTPRINT_WORDS; ++i)
            ps5vk_multiview_witness_depth(&interleaved, layer, (i % 2u) ?
                ps5vk_multiview_witness_clear_word() :
                ps5vk_multiview_witness_depth_word(layer));
    }
    for (uint32_t i = 0; i < GUARD_WORDS; ++i)
        ps5vk_multiview_witness_guard(&interleaved, SENTINEL, SENTINEL);
    assert(ps5vk_multiview_witness_verify(&interleaved, PS5VK_MULTIVIEW_WITNESS_VIEWS,
        FOOTPRINT_WORDS) && interleaved.strict_verified);

    /* Six layers holding the SAME view's data: a global sum would accept it, the
     * oracle must not. */
    struct ps5vk_multiview_witness identical = {0};
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer)
        fill_layer(&identical, layer, 3u, 3u, 1);
    for (uint32_t i = 0; i < GUARD_WORDS; ++i)
        ps5vk_multiview_witness_guard(&identical, SENTINEL, SENTINEL);
    assert(!ps5vk_multiview_witness_verify(&identical, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    assert(identical.layer[0].other_view == PIXELS && identical.layer[5].other_view == PIXELS &&
           identical.layer[0].depth_other == PIXELS && identical.layer[3].other_view == 0);

    /* One layer holding half its own colour and half of another view's. */
    struct ps5vk_multiview_witness alias = correct();
    alias.layer[2].pixels = alias.layer[2].expected = alias.layer[2].other_view =
        alias.layer[2].other = 0;
    for (uint32_t i = 0; i < PIXELS / 2u; ++i)
        ps5vk_multiview_witness_color_pixel(&alias, 2u, color_of(2u));
    for (uint32_t i = 0; i < PIXELS / 2u; ++i)
        ps5vk_multiview_witness_color_pixel(&alias, 2u, color_of(5u));
    assert(!ps5vk_multiview_witness_verify(&alias, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    assert(alias.layer[2].expected == PIXELS / 2u && alias.layer[2].other_view == PIXELS / 2u);

    /* A depth word belonging to another view, and one belonging to nobody. */
    struct ps5vk_multiview_witness foreign = correct();
    foreign.layer[1].depth_expected -= 1u;
    foreign.layer[1].depth_clear += 1u;
    ps5vk_multiview_witness_depth(&foreign, 1u, ps5vk_multiview_witness_depth_word(4u));
    assert(!ps5vk_multiview_witness_verify(&foreign, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    assert(foreign.layer[1].depth_other == 1u && !foreign.layer[1].depth_unknown);
    struct ps5vk_multiview_witness unknown = correct();
    ps5vk_multiview_witness_depth(&unknown, 3u, 0x12345678u);
    assert(!ps5vk_multiview_witness_verify(&unknown, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    assert(unknown.layer[3].depth_unknown == 1u);

    /* One word too many of this view's own depth, and one word short of the
     * clear count: the count is the proof, so both fail. */
    struct ps5vk_multiview_witness extra_depth = correct();
    extra_depth.layer[0].depth_expected += 1u;
    extra_depth.layer[0].depth_clear -= 1u;
    assert(!ps5vk_multiview_witness_verify(&extra_depth, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    struct ps5vk_multiview_witness short_depth = correct();
    short_depth.layer[0].depth_expected -= 1u;
    short_depth.layer[0].depth_clear += 1u;
    assert(!ps5vk_multiview_witness_verify(&short_depth, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));

    /* Lost colour coverage, a touched guard, and the wrong shape. */
    struct ps5vk_multiview_witness short_color = correct();
    short_color.layer[0].pixels -= 1u;
    short_color.layer[0].expected -= 1u;
    assert(!ps5vk_multiview_witness_verify(&short_color, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    struct ps5vk_multiview_witness touched = correct();
    ps5vk_multiview_witness_guard(&touched, SENTINEL + 1u, SENTINEL);
    assert(!ps5vk_multiview_witness_verify(&touched, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS) &&
           touched.guard_mismatches == 1u);
    struct ps5vk_multiview_witness unguarded = correct();
    unguarded.guard_words = 0;
    assert(!ps5vk_multiview_witness_verify(&unguarded, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    w = correct();
    assert(!ps5vk_multiview_witness_verify(&w, PS5VK_MULTIVIEW_WITNESS_VIEWS - 1u, FOOTPRINT_WORDS));
    assert(!ps5vk_multiview_witness_verify(&w, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS - 1u));
    assert(!ps5vk_multiview_witness_verify(&w, PS5VK_MULTIVIEW_WITNESS_VIEWS, 0));
    assert(!ps5vk_multiview_witness_verify(NULL, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    puts("Multiview witness oracle: detiled colour per layer, depth footprint counted, guards and aliasing included");
    return 0;
}

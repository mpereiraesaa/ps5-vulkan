/* The six-view witness oracle, on the host. Everything the native probe will
 * judge its readback with is exercised here first: the expectations themselves,
 * a fully correct scene, and one negative per way a layer can be wrong - a
 * foreign colour (aliasing), a foreign or unknown depth, lost coverage, a touched
 * guard - plus the case that matters most for the review: six layers that all
 * hold the SAME data, which any global sum would accept and this oracle does
 * not. */
#include "multiview_witness.h"
#include <assert.h>
#include <stdio.h>

/* A tiny layer standing in for the 64x64 the probe reads: the oracle counts
 * pixels, so its arithmetic is the same at any size. */
enum { PIXELS = 8, GUARD_WORDS = 4, SENTINEL = 0x5a5a5a5a };

static struct ps5vk_multiview_witness correct(void)
{
    struct ps5vk_multiview_witness w = {0};
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer) {
        uint8_t rgba[4];
        ps5vk_multiview_witness_color(layer, rgba);
        for (uint32_t i = 0; i < PIXELS; ++i)
            ps5vk_multiview_witness_pixel(&w, layer, rgba,
                ps5vk_multiview_witness_depth_word(layer));
    }
    for (uint32_t i = 0; i < GUARD_WORDS; ++i)
        ps5vk_multiview_witness_guard(&w, SENTINEL, SENTINEL);
    return w;
}

/* One layer filled with a colour and depth of the caller's choosing. */
static void fill(struct ps5vk_multiview_witness *w, uint32_t layer, uint32_t color_view,
    uint32_t depth_view, uint32_t pixels)
{
    uint8_t rgba[4];
    ps5vk_multiview_witness_color(color_view, rgba);
    for (uint32_t i = 0; i < pixels; ++i)
        ps5vk_multiview_witness_pixel(w, layer, rgba,
            ps5vk_multiview_witness_depth_word(depth_view));
}

int main(void)
{
    /* The expectations are exact and every view is distinguishable: the colours
     * are the UNORM8 bytes of red = (view+1)/16 and green = view/8, and the depth
     * words are the float bits of (view+1)/16. */
    const uint8_t red[PS5VK_MULTIVIEW_WITNESS_VIEWS] = {16, 32, 48, 64, 80, 96};
    const uint8_t green[PS5VK_MULTIVIEW_WITNESS_VIEWS] = {0, 32, 64, 96, 128, 160};
    const uint32_t depth[PS5VK_MULTIVIEW_WITNESS_VIEWS] = {
        0x3d800000u, 0x3e000000u, 0x3e400000u, 0x3e800000u, 0x3ea00000u, 0x3ec00000u};
    for (uint32_t view = 0; view < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++view) {
        uint8_t rgba[4];
        ps5vk_multiview_witness_color(view, rgba);
        assert(rgba[0] == red[view] && rgba[1] == green[view] &&
               rgba[2] == 128u && rgba[3] == 255u);
        assert(ps5vk_multiview_witness_depth_word(view) == depth[view]);
        assert(ps5vk_multiview_witness_view(view) == view);
    }
    for (uint32_t a = 0; a < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++a)
        for (uint32_t b = a + 1; b < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++b) {
            uint8_t ca[4], cb[4];
            ps5vk_multiview_witness_color(a, ca);
            ps5vk_multiview_witness_color(b, cb);
            assert(ca[0] != cb[0] && ca[1] != cb[1]);
            assert(depth[a] != depth[b]);
        }

    /* A correct scene verifies. */
    struct ps5vk_multiview_witness w = correct();
    assert(ps5vk_multiview_witness_verify(&w, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS) &&
           w.strict_verified);
    assert(w.guard_words == GUARD_WORDS && !w.guard_mismatches);
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer)
        assert(w.layer[layer].expected == PIXELS && w.layer[layer].depth_expected == PIXELS);

    /* The case a global sum would accept and this oracle must not: every layer
     * holding the same view's data. Each layer then holds a foreign colour, and
     * the verdict is a failure with the counts saying exactly that. */
    w = correct();
    struct ps5vk_multiview_witness identical = {0};
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer)
        fill(&identical, layer, 3u, 3u, PIXELS);
    for (uint32_t i = 0; i < GUARD_WORDS; ++i)
        ps5vk_multiview_witness_guard(&identical, SENTINEL, SENTINEL);
    assert(!ps5vk_multiview_witness_verify(&identical, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS) &&
           !identical.strict_verified);
    assert(identical.layer[3].expected == PIXELS && identical.layer[3].other_view == 0);
    assert(identical.layer[0].other_view == PIXELS && identical.layer[5].other_view == PIXELS);

    /* Aliasing: one layer holds half its own pixels and half of another view's. */
    w = correct();
    struct ps5vk_multiview_witness alias = {0};
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer)
        fill(&alias, layer, layer, layer, PIXELS);
    alias.layer[2].pixels = alias.layer[2].expected = alias.layer[2].depth_expected = 0;
    fill(&alias, 2u, 2u, 2u, PIXELS / 2u);
    fill(&alias, 2u, 5u, 2u, PIXELS / 2u);
    for (uint32_t i = 0; i < GUARD_WORDS; ++i)
        ps5vk_multiview_witness_guard(&alias, SENTINEL, SENTINEL);
    assert(!ps5vk_multiview_witness_verify(&alias, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS));
    assert(alias.layer[2].pixels == PIXELS && alias.layer[2].expected == PIXELS / 2u &&
           alias.layer[2].other_view == PIXELS / 2u && alias.layer[2].depth_expected == PIXELS);

    /* A layer whose depth belongs to another view, and one whose depth belongs
     * to nobody: the colour is perfect in both. */
    struct ps5vk_multiview_witness foreign_depth = alias;
    foreign_depth.layer[2] = w.layer[2];
    ps5vk_multiview_witness_pixel(&foreign_depth, 2u, (const uint8_t[]){red[2], green[2], 128u, 255u},
        ps5vk_multiview_witness_depth_word(4u));
    assert(!ps5vk_multiview_witness_verify(&foreign_depth, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS));
    assert(foreign_depth.layer[2].depth_other == 1u);
    struct ps5vk_multiview_witness unknown_depth = correct();
    ps5vk_multiview_witness_pixel(&unknown_depth, 1u, (const uint8_t[]){red[1], green[1], 128u, 255u},
        0x12345678u);
    assert(!ps5vk_multiview_witness_verify(&unknown_depth, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS));
    assert(unknown_depth.layer[1].depth_unknown == 1u && !unknown_depth.layer[1].depth_other);

    /* Lost coverage: a layer one pixel short of full has no verdict either. */
    struct ps5vk_multiview_witness short_layer = correct();
    short_layer.layer[0].pixels -= 1u;
    short_layer.layer[0].expected -= 1u;
    short_layer.layer[0].depth_expected -= 1u;
    assert(!ps5vk_multiview_witness_verify(&short_layer, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS));

    /* A touched guard word, and a witness with no guard words at all. */
    struct ps5vk_multiview_witness touched = correct();
    ps5vk_multiview_witness_guard(&touched, SENTINEL + 1u, SENTINEL);
    assert(!ps5vk_multiview_witness_verify(&touched, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS) &&
           touched.guard_mismatches == 1u);
    struct ps5vk_multiview_witness unguarded = correct();
    unguarded.guard_words = 0;
    assert(!ps5vk_multiview_witness_verify(&unguarded, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS));

    /* The wrong number of layers, and an impossible layer size, never verify. */
    w = correct();
    assert(!ps5vk_multiview_witness_verify(&w, PS5VK_MULTIVIEW_WITNESS_VIEWS - 1u, PIXELS));
    assert(!ps5vk_multiview_witness_verify(&w, PS5VK_MULTIVIEW_WITNESS_VIEWS + 1u, PIXELS));
    assert(!ps5vk_multiview_witness_verify(&w, PS5VK_MULTIVIEW_WITNESS_VIEWS, 0));
    assert(!ps5vk_multiview_witness_verify(NULL, PS5VK_MULTIVIEW_WITNESS_VIEWS, PIXELS));
    puts("Multiview witness oracle: six layers classified per layer, guards and aliasing included");
    return 0;
}

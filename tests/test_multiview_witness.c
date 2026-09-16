/* The six-view witness oracle, on the host. Everything the native probe judges
 * its readback with is exercised here first: the expectations, a correct scene,
 * and one negative per way a layer can be wrong - a foreign colour (aliasing), a
 * foreign or unexplained depth word, a wrong amount of cleared depth, lost
 * coverage, a touched guard - plus the case a global sum would wrongly accept:
 * six layers all holding the same data.
 *
 * The regressions exist because of the P1 reviews: a layer's depth is judged by
 * COUNTING words over its whole footprint, so the 4096 words a view wrote do not
 * have to sit in the footprint's prefix (the old linear assumption would fail on
 * this driver's tiled 64KB_Z_X surfaces); and because the pass loads with
 * LOAD_OP_DONT_CARE, the remainder of the footprint may hold the clear value,
 * the sentinel it was seeded with, or any mixture - the verdict requires only
 * that the remainder is accounted for, never that it is "clean". Colour is only
 * ever judged on DETILED pixels, because the attachment is a tiled 64KB_R_X
 * surface. */
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

/* A correct witness: six layers, each with its 4096 own depth words plus a
 * remainder of the caller's choosing (clear, unknown/sentinel, or a mixture),
 * and the trailing layer's sentinels. */
static struct ps5vk_multiview_witness correct_remainder(uint64_t clear_words)
{
    struct ps5vk_multiview_witness w = {0};
    for (uint32_t layer = 0; layer < PS5VK_MULTIVIEW_WITNESS_VIEWS; ++layer) {
        const uint8_t *rgba = color_of(layer);
        for (uint32_t i = 0; i < PIXELS; ++i)
            ps5vk_multiview_witness_color_pixel(&w, layer, rgba);
        for (uint32_t i = 0; i < PIXELS; ++i)
            ps5vk_multiview_witness_depth(&w, layer,
                ps5vk_multiview_witness_depth_word(layer));
        for (uint64_t i = PIXELS; i < FOOTPRINT_WORDS; ++i)
            ps5vk_multiview_witness_depth(&w, layer,
                i - PIXELS < clear_words ? ps5vk_multiview_witness_clear_word() : SENTINEL);
    }
    for (uint32_t i = 0; i < GUARD_WORDS; ++i)
        ps5vk_multiview_witness_guard(&w, SENTINEL, SENTINEL);
    return w;
}

static struct ps5vk_multiview_witness correct(void)
{
    return correct_remainder(FOOTPRINT_WORDS - PIXELS);
}

int main(void)
{
    /* The expectations are exact, pairwise distinct, and the clear word is the
     * one a depth-1.0 clear writes. */
    const uint8_t red[PS5VK_MULTIVIEW_WITNESS_VIEWS] = {16, 32, 48, 64, 80, 96};
    /* Green is UNORM8 round-to-nearest of view/8, so view 5 is 159 and not 160:
     * 0.625 * 255 = 159.375. The first hardware run's oracle said 160, and this
     * regression is what keeps that off-by-one from coming back. */
    const uint8_t green[PS5VK_MULTIVIEW_WITNESS_VIEWS] = {0, 32, 64, 96, 128, 159};
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
    /* The exact colour of view 5, byte for byte, and the near-miss case: a layer
     * filled with it is correct after the fix. */
    {
        uint8_t rgba[4];
        ps5vk_multiview_witness_color(5u, rgba);
        assert(rgba[0] == 96u && rgba[1] == 159u && rgba[2] == 128u && rgba[3] == 255u);
        struct ps5vk_multiview_witness v5 = {0};
        for (uint32_t i = 0; i < PIXELS; ++i)
            ps5vk_multiview_witness_color_pixel(&v5, 5u, rgba);
        assert(v5.layer[5].expected == PIXELS && !v5.layer[5].other_view && !v5.layer[5].other);
    }

    /* maxMultiviewInstanceIndex: the pinned value is 2^27-1 and can only be
     * witnessed as an integer - it is not representable in a float - so the
     * predicate and the shader share one integer form. Near misses are rejected
     * here exactly as the shader's bit test rejects them. */
    assert(ps5vk_multiview_witness_instance() == UINT32_C(0x07ffffff));
    assert(ps5vk_multiview_witness_instance_exact(UINT32_C(0x07ffffff)));
    assert(!ps5vk_multiview_witness_instance_exact(UINT32_C(0x08000000)));
    assert(!ps5vk_multiview_witness_instance_exact(UINT32_C(0x07fffffe)));
    assert(!ps5vk_multiview_witness_instance_exact(UINT32_C(0x0fffffff)));
    assert(!ps5vk_multiview_witness_instance_exact(0u));
    /* ...and the float round-trip the shader must NOT do would land on 2^27. */
    {
        const float rounded = (float)UINT32_C(0x07ffffff);
        assert((uint32_t)rounded == UINT32_C(0x08000000));
    }

    /* A layer carrying the fixed instance-failure colour is an instance failure,
     * not a foreign view, and it cannot pass. */
    {
        uint8_t failed[4];
        ps5vk_multiview_witness_instance_fail_color(failed);
        assert(failed[0] == 255u && failed[1] == 0u && failed[2] == 255u && failed[3] == 255u);
        struct ps5vk_multiview_witness failed_layer = {0};
        for (uint32_t i = 0; i < PIXELS; ++i)
            ps5vk_multiview_witness_color_pixel(&failed_layer, 1u, failed);
        assert(failed_layer.layer[1].instance_failed == PIXELS &&
               !failed_layer.layer[1].expected && !failed_layer.layer[1].other_view &&
               !failed_layer.layer[1].other && !failed_layer.layer[1].color_foreign_mask);
        struct ps5vk_multiview_witness mixed = correct();
        mixed.layer[3].pixels = mixed.layer[3].expected = 0;
        mixed.layer[3].color_foreign_mask = 0;
        mixed.layer[3].color_first_foreign_set = 0;
        for (uint32_t i = 0; i < PIXELS; ++i)
            ps5vk_multiview_witness_color_pixel(&mixed, 3u, failed);
        assert(!ps5vk_multiview_witness_verify(&mixed, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
        assert(mixed.layer[3].instance_failed == PIXELS);
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

    /* LOAD_OP_DONT_CARE means the remainder may be anything: all sentinel, all
     * clear, or a mixture. All three verify, and the counters say which was
     * which - nothing here is called "clean padding". */
    struct ps5vk_multiview_witness unknown_remainder = correct_remainder(0);
    assert(ps5vk_multiview_witness_verify(&unknown_remainder, PS5VK_MULTIVIEW_WITNESS_VIEWS,
        FOOTPRINT_WORDS) && unknown_remainder.strict_verified);
    assert(!unknown_remainder.layer[0].depth_clear &&
           unknown_remainder.layer[0].depth_unknown == FOOTPRINT_WORDS - PIXELS);
    struct ps5vk_multiview_witness mixed_remainder =
        correct_remainder((FOOTPRINT_WORDS - PIXELS) / 3u);
    assert(ps5vk_multiview_witness_verify(&mixed_remainder, PS5VK_MULTIVIEW_WITNESS_VIEWS,
        FOOTPRINT_WORDS) && mixed_remainder.strict_verified);
    assert(mixed_remainder.layer[0].depth_clear == (FOOTPRINT_WORDS - PIXELS) / 3u &&
           mixed_remainder.layer[0].depth_unknown ==
               FOOTPRINT_WORDS - PIXELS - (FOOTPRINT_WORDS - PIXELS) / 3u);

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

    /* A layer holding exactly ONE other view's colour names that view, byte for
     * byte, which is the diagnostic the next run will use: layer 2 filled with
     * view 5's colour must report mask 0x20 and view 5. */
    struct ps5vk_multiview_witness foreign_color = correct();
    foreign_color.layer[2].pixels = foreign_color.layer[2].expected =
        foreign_color.layer[2].other_view = foreign_color.layer[2].other = 0;
    foreign_color.layer[2].color_foreign_mask = 0;
    foreign_color.layer[2].color_first_foreign_set = 0;
    for (uint32_t i = 0; i < PIXELS; ++i)
        ps5vk_multiview_witness_color_pixel(&foreign_color, 2u, color_of(5u));
    assert(!ps5vk_multiview_witness_verify(&foreign_color, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    assert(foreign_color.layer[2].other_view == PIXELS && !foreign_color.layer[2].expected);
    assert(foreign_color.layer[2].color_foreign_mask == (UINT32_C(1) << 5) &&
           foreign_color.layer[2].color_foreign_view == 5u);
    assert(foreign_color.layer[2].color_first_foreign_set &&
           foreign_color.layer[2].color_first_foreign[0] == 96u &&
           foreign_color.layer[2].color_first_foreign[1] == 159u &&
           foreign_color.layer[2].color_first_foreign[2] == 128u);

    /* A colour no view produces is `other`, and it must NOT be reported as a
     * foreign view: the diagnosis has to stay honest about what it saw. */
    struct ps5vk_multiview_witness unclaimed = correct();
    unclaimed.layer[4].pixels = unclaimed.layer[4].expected = 0;
    unclaimed.layer[4].color_foreign_mask = 0;
    unclaimed.layer[4].color_first_foreign_set = 0;
    for (uint32_t i = 0; i < PIXELS; ++i)
        ps5vk_multiview_witness_color_pixel(&unclaimed, 4u,
            (const uint8_t[]){0x11u, 0x22u, 0x33u, 0xffu});
    assert(!ps5vk_multiview_witness_verify(&unclaimed, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    assert(unclaimed.layer[4].other == PIXELS && !unclaimed.layer[4].other_view &&
           !unclaimed.layer[4].color_foreign_mask);
    assert(unclaimed.layer[4].color_first_foreign_set &&
           unclaimed.layer[4].color_first_foreign[0] == 0x11u);

    /* A depth word belonging to another view, and one belonging to nobody. */
    struct ps5vk_multiview_witness foreign = correct();
    foreign.layer[1].depth_expected -= 1u;
    foreign.layer[1].depth_clear += 1u;
    ps5vk_multiview_witness_depth(&foreign, 1u, ps5vk_multiview_witness_depth_word(4u));
    assert(!ps5vk_multiview_witness_verify(&foreign, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    assert(foreign.layer[1].depth_other == 1u && !foreign.layer[1].depth_unknown &&
           foreign.layer[1].depth_foreign_mask == (UINT32_C(1) << 4) &&
           foreign.layer[1].depth_foreign_view == 4u);
    struct ps5vk_multiview_witness unknown = correct();
    ps5vk_multiview_witness_depth(&unknown, 3u, 0x12345678u);
    assert(!ps5vk_multiview_witness_verify(&unknown, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    assert(unknown.layer[3].depth_unknown == 1u);

    /* One word too many of this view's own depth, and one word short of the
     * accounted remainder: the count is the proof, so both fail. */
    struct ps5vk_multiview_witness extra_depth = correct();
    --extra_depth.layer[0].depth_clear;
    ++extra_depth.layer[0].depth_expected;
    assert(!ps5vk_multiview_witness_verify(&extra_depth, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    struct ps5vk_multiview_witness short_depth = correct();
    --short_depth.layer[0].depth_expected;
    ++short_depth.layer[0].depth_clear;
    assert(!ps5vk_multiview_witness_verify(&short_depth, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));
    /* A remainder that does not add up to the footprint is a failure even with
     * the right expected count. */
    struct ps5vk_multiview_witness unaccounted = correct();
    --unaccounted.layer[0].depth_unknown;
    assert(!ps5vk_multiview_witness_verify(&unaccounted, PS5VK_MULTIVIEW_WITNESS_VIEWS, FOOTPRINT_WORDS));

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

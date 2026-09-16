#ifndef PS5VK_MULTIVIEW_WITNESS_H
#define PS5VK_MULTIVIEW_WITNESS_H
#include <stdint.h>

/* The private six-view witness (T02-D1b). These are the expectations a layer of
 * the readback is judged against, and they are exact rather than approximate:
 * the witness vertex stage writes red = (view+1)/16, green = view/8, blue = 1/2
 * and alpha = 1, every one of which is exact in UNORM8, and depth = (view+1)/16,
 * which is exact in D32. A layer therefore either holds its own value or it does
 * not.
 *
 * The oracle never sums across layers. Every pixel is classified against ITS
 * layer's own colour first and then against every other view's, so a layer that
 * rendered another view's data is visible as itself holding foreign pixels -
 * which is what aliasing between layers looks like - and the depth of each layer
 * is checked independently of its colour. */
enum { PS5VK_MULTIVIEW_WITNESS_VIEWS = 6,
       /* One layer of the witness is 64x64 pixels. */
       PS5VK_MULTIVIEW_WITNESS_PIXELS = 64 * 64 };

/* The view a witness layer is expected to hold: 0..5, the bits of a six-view
 * mask in ascending order. */
uint32_t ps5vk_multiview_witness_view(uint32_t layer);

/* The colour of one view in canonical RGBA byte order, and its D32 word. */
void ps5vk_multiview_witness_color(uint32_t view, uint8_t rgba[4]);
uint32_t ps5vk_multiview_witness_depth_word(uint32_t view);
/* The D32 word a render pass CLEAR of depth 1.0 writes, which is what a word of
 * a layer's footprint holds when the view did not write it. */
uint32_t ps5vk_multiview_witness_clear_word(void);

/* What one layer of the readback contains.
 *
 * COLOUR is judged on the layer's 4096 pixels, but only after the real detile:
 * a colour attachment here is a tiled 64KB_R_X surface, so reading its first
 * words as if they were linear pixels is meaningless. `pixels`, `expected`,
 * `other_view` and `other` count those detiled pixels.
 *
 * DEPTH is counted over the layer's WHOLE footprint, not per pixel. The witness
 * vertex stage writes one UNIFORM depth per view, which is tiling-invariant, so
 * the count is the coverage proof: exactly 4096 words hold this view's depth and
 * no word holds another view's. The rest of the footprint is REMainder the pass
 * does not write: the pass loads it with LOAD_OP_DONT_CARE, so Vulkan guarantees
 * nothing about that content and the verdict does not depend on it. The two
 * remainder counters below are reported honestly - `depth_clear` for words that
 * happen to hold the clear value and `depth_unknown` for everything else - and
 * only their SUM is required, because DONT_CARE is allowed to be anything. This
 * driver does not have the 64KB_Z_X pixel equations, so no per-pixel depth
 * coordinate is claimed anywhere. */
struct ps5vk_multiview_layer_witness {
    uint64_t pixels, expected, other_view, other;
    uint64_t depth_expected, depth_other, depth_clear, depth_unknown;
    /* Fail-closed diagnosis, so a foreign colour is identifiable rather than
     * guessed at: the first detiled pixel that was not this layer's own, the set
     * of foreign views whose colour or depth was seen, and the single view index
     * when everything foreign belonged to the same candidate (0xff otherwise). */
    uint32_t color_foreign_mask, color_foreign_view;
    uint8_t color_first_foreign[4], color_first_foreign_set;
    uint32_t depth_foreign_mask, depth_foreign_view;
};

struct ps5vk_multiview_witness {
    uint32_t views;
    uint64_t pixels_per_layer;
    uint64_t depth_footprint_words;
    uint64_t guard_words, guard_mismatches;
    struct ps5vk_multiview_layer_witness layer[PS5VK_MULTIVIEW_WITNESS_VIEWS];
    int strict_verified;
};

/* Classify one DETILED colour pixel of one layer, in canonical RGBA order. */
void ps5vk_multiview_witness_color_pixel(struct ps5vk_multiview_witness *w,
    uint32_t view, const uint8_t rgba[4]);

/* Classify one word of one layer's depth footprint: this view's depth, another
 * view's depth, the clear value, or something unexplained. */
void ps5vk_multiview_witness_depth(struct ps5vk_multiview_witness *w, uint32_t view,
    uint32_t word);

/* Fold one guard word: the part of a layer's storage the render must not touch,
 * including the trailing layer beyond the views the pass renders. */
void ps5vk_multiview_witness_guard(struct ps5vk_multiview_witness *w, uint32_t word,
    uint32_t sentinel);

/* The verdict, with `depth_footprint_words` the number of words one layer of the
 * depth attachment occupies (its stride divided by four). A layer verifies only
 * if its 4096 detiled pixels are all its own colour, its depth footprint holds
 * exactly 4096 words of its own depth, no word of another view's depth, the
 * remainder is accounted for (clear + unexplained == footprint - 4096, whatever
 * LOAD_OP_DONT_CARE chose to leave there), the six layers are therefore
 * distinct, and every guard word still holds the sentinel it was seeded with.
 * Returns 1 and sets strict_verified, or returns 0 with the per-layer counts
 * left in place for the caller to report. */
int ps5vk_multiview_witness_verify(struct ps5vk_multiview_witness *w, uint32_t views,
    uint64_t depth_footprint_words);

#endif

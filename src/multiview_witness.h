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
enum { PS5VK_MULTIVIEW_WITNESS_VIEWS = 6 };

/* The view a witness layer is expected to hold: 0..5, the bits of a six-view
 * mask in ascending order. */
uint32_t ps5vk_multiview_witness_view(uint32_t layer);

/* The colour of one view in canonical RGBA byte order, and its D32 word. */
void ps5vk_multiview_witness_color(uint32_t view, uint8_t rgba[4]);
uint32_t ps5vk_multiview_witness_depth_word(uint32_t view);

/* What one layer of the readback contains. `pixels` counts the pixels examined,
 * `expected` those holding this layer's own colour, `other_view` those holding
 * another view's colour, and `other` everything else. Depth is counted the same
 * way and independently of colour. */
struct ps5vk_multiview_layer_witness {
    uint64_t pixels, expected, other_view, other;
    uint64_t depth_expected, depth_other, depth_unknown;
};

struct ps5vk_multiview_witness {
    uint32_t views;
    uint64_t pixels_per_layer;
    uint64_t guard_words, guard_mismatches;
    struct ps5vk_multiview_layer_witness layer[PS5VK_MULTIVIEW_WITNESS_VIEWS];
    int strict_verified;
};

/* Classify one pixel of one layer. `rgba` is in canonical RGBA order (the
 * readback boundary converts the target's own byte order) and `depth` is the raw
 * D32 word exactly as it came back. */
void ps5vk_multiview_witness_pixel(struct ps5vk_multiview_witness *w, uint32_t view,
    const uint8_t rgba[4], uint32_t depth);

/* Fold one guard word: the part of a layer's storage the render must not touch,
 * including the trailing layer beyond the views the pass renders. */
void ps5vk_multiview_witness_guard(struct ps5vk_multiview_witness *w, uint32_t word,
    uint32_t sentinel);

/* The verdict. With every pixel of every layer classified and every guard word
 * folded in, a witness verifies only if each layer is FULLY covered by its own
 * colour, holds its own depth everywhere that colour landed, contains no pixel of
 * another view's colour and no depth word belonging to another view, the six
 * layers are therefore distinct, and every guard word still holds the sentinel it
 * was seeded with. Returns 1 and sets strict_verified, or returns 0 with the
 * per-layer counts left in place for the caller to report. */
int ps5vk_multiview_witness_verify(struct ps5vk_multiview_witness *w, uint32_t views,
    uint64_t pixels_per_layer);

#endif

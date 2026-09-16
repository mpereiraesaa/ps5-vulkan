/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Clip/cull distance coverage oracle.
 *
 * One triangle pair covers the whole target with a varying that is affine in
 * the position, and the distance under test is a function of the same position.
 * The oracle predicts, for every pixel centre, whether the distance predicate
 * keeps it and which colour the interpolated varying must produce there, so a
 * wrong result cannot be mistaken for a plausible one:
 *
 *  - a clip distance removes the fragments past its plane and keeps the rest,
 *    interpolated as if the primitive had been cut;
 *  - a cull distance is a per-half-space primitive decision: a primitive is
 *    discarded when one cull distance index is negative for every vertex, and
 *    is not disturbed at all when the negativity is spread across the vertices
 *    (the rule the pinned CTS states in its negative_and_non_negative case).
 *
 * The same oracle is driven by the host regression and by the native witness,
 * so the pixel rule is one implementation with two kinds of evidence.
 */
#ifndef PS5VK_CLIP_CULL_WITNESS_H
#define PS5VK_CLIP_CULL_WITNESS_H
#include <stdint.h>

enum {
    /* Control: the same geometry with no distance declared at all. */
    PS5VK_CLIP_CULL_PLAIN = 0,
    /* Every declared distance stays positive: coverage must equal the control. */
    PS5VK_CLIP_CULL_POSITIVE = 1,
    /* Clip distance 0 is the x coordinate: the left half is cut away. */
    PS5VK_CLIP_CULL_CLIP_HALF = 2,
    /* Clip distances 0 and 1 are x and y: one quadrant survives. */
    PS5VK_CLIP_CULL_CLIP_QUADRANT = 3,
    /* Cull distance 0 is the x coordinate: negative at one vertex only, so the
     * primitive must survive untouched - culling is not clipping. */
    PS5VK_CLIP_CULL_CULL_HALF = 4,
    /* Every cull distance is negative at every vertex: discarded whole. */
    PS5VK_CLIP_CULL_CULL_NEGATIVE = 5,
    /* The quadrant clip with both cull distances exported and positive. */
    PS5VK_CLIP_CULL_MIXED = 6,
    /* Cull index 0 is mixed and index 1 is negative everywhere: still
     * discarded, because the rule is per half-space rather than per vertex. */
    PS5VK_CLIP_CULL_CULL_INDEX = 7,
    PS5VK_CLIP_CULL_CASES = 8
};

struct ps5vk_clip_cull_witness {
    uint64_t pixels;
    /* Pixels the case's distance predicate keeps. */
    uint64_t expected_covered;
    /* Of those, the ones that really carry the interpolated varying. */
    uint64_t covered;
    /* Predicate pixels that were left at the clear colour. */
    uint64_t missing;
    /* Pixels the predicate rejects that were written anyway. */
    uint64_t foreign;
    /* Covered pixels whose colour is not the interpolated varying. */
    uint64_t wrong_color;
    uint8_t first_foreign[4];
    unsigned first_foreign_x, first_foreign_y;
    uint8_t first_wrong[4];
    unsigned first_wrong_x, first_wrong_y;
};

/* The clear colour the target is left with where no fragment was written. */
extern const uint8_t ps5vk_clip_cull_clear[4];

/* Classify one pixel of one rendered case. `extent` is the target's side. */
void ps5vk_clip_cull_witness_pixel(struct ps5vk_clip_cull_witness *witness,
    unsigned witness_case,unsigned x,unsigned y,unsigned extent,const uint8_t rgba[4]);

/* 1 when the case produced exactly the coverage and colours it must. */
int ps5vk_clip_cull_witness_verify(const struct ps5vk_clip_cull_witness *witness,
    unsigned witness_case,unsigned extent);

/* The varying the witness vertex stage writes for a vertex, and the colours it
 * must interpolate to at a pixel centre. Exposed so the host regression can
 * build the same coordinates the native probe reads back. */
void ps5vk_clip_cull_witness_expected(unsigned x,unsigned y,unsigned extent,uint8_t rgba[4]);

#endif

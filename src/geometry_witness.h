/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Geometry-shader coverage oracle.
 *
 * The witness vertex stage emits the two triangles that tile the whole target
 * and gives them a varying affine in the position. The geometry stage under
 * test then either passes a primitive through, shrinks its vertices, suppresses
 * it or rewrites the varying, and the oracle predicts every pixel from the same
 * analytic geometry: inside the union of the two (possibly scaled) triangles,
 * the interpolated varying must appear; outside it the target must still hold
 * the clear colour.
 *
 *  - passthrough must reproduce the two-stage control image pixel for pixel;
 *  - shrink must cover exactly the centred square the scaled triangles tile;
 *  - suppress must leave the target clear, which a vertex stage cannot do;
 *  - the varying rewrite must keep the coverage and change every colour, so the
 *    fragment stage is proven to read what the geometry stage wrote.
 *
 * The same oracle is driven by the host regression and by the native witness.
 */
#ifndef PS5VK_GEOMETRY_WITNESS_H
#define PS5VK_GEOMETRY_WITNESS_H
#include <stdint.h>

enum {
    /* The two-stage pipeline with no geometry stage: the baseline image. */
    PS5VK_GEOMETRY_CONTROL = 0,
    /* Geometry modes, selected by the specialization constant the stage reads. */
    PS5VK_GEOMETRY_PASSTHROUGH = 1,
    PS5VK_GEOMETRY_SHRINK = 2,
    PS5VK_GEOMETRY_SUPPRESS = 3,
    PS5VK_GEOMETRY_RECOLOR = 4,
    /* One invocation emits three sub-triangles for every input primitive: the
     * union must be the original triangle, pixel for pixel. */
    PS5VK_GEOMETRY_AMPLIFY = 5,
    /* A fixed centred quad with a fixed colour, emitted without reading the
     * input: it isolates "the stage ran" from "the stage received the vertex
     * data", which is what a broken ES/GS handshake looks like. */
    PS5VK_GEOMETRY_CONSTANT = 6,
    PS5VK_GEOMETRY_CASES = 7
};
/* The geometry stage's mode for a case: -1 means the control has no geometry
 * stage at all. */
int ps5vk_geometry_witness_mode(unsigned witness_case);

struct ps5vk_geometry_witness {
    uint64_t pixels;
    uint64_t expected_covered;
    uint64_t covered;
    uint64_t missing;
    uint64_t foreign;
    uint64_t wrong_color;
    uint8_t first_foreign[4];
    unsigned first_foreign_x, first_foreign_y;
    uint8_t first_wrong[4];
    unsigned first_wrong_x, first_wrong_y;
};

/* The clear colour the target is left with where no fragment was written. */
extern const uint8_t ps5vk_geometry_clear[4];

/* Classify one pixel of one rendered case. `extent` is the target's side. */
void ps5vk_geometry_witness_pixel(struct ps5vk_geometry_witness *witness,
    unsigned witness_case,unsigned x,unsigned y,unsigned extent,const uint8_t rgba[4]);

/* 1 when the case produced exactly the coverage and colours it must. */
int ps5vk_geometry_witness_verify(const struct ps5vk_geometry_witness *witness,
    unsigned witness_case,unsigned extent);

/* The interpolated varying at a pixel centre, which is what a covered pixel
 * must carry (after the geometry stage's own rewrite, if it has one). */
void ps5vk_geometry_witness_expected(unsigned witness_case,unsigned x,unsigned y,
    unsigned extent,uint8_t rgba[4]);

#endif

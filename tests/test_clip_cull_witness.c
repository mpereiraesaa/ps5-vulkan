/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host regression for the clip/cull coverage oracle. The oracle decides whether
 * a native readback shows clipping, culling or neither, so its own arithmetic
 * and every way it can be fooled are exercised here first: a synthetic image is
 * built from the predicate, and then exactly one pixel is broken per negative.
 */
#include "clip_cull_witness.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { EXTENT = 64 };

static void image_for(unsigned witness_case,uint8_t *image)
{
    for(unsigned y=0;y<EXTENT;++y)for(unsigned x=0;x<EXTENT;++x) {
        uint8_t *pixel=image+4*((size_t)y*EXTENT+x);
        /* Start from the clear colour and write only what the case keeps. */
        memcpy(pixel,ps5vk_clip_cull_clear,4);
        struct ps5vk_clip_cull_witness probe={0};
        uint8_t expected[4];
        ps5vk_clip_cull_witness_expected(witness_case,x,y,EXTENT,expected);
        /* Reuse the oracle's predicate through a one-pixel classification: the
         * synthetic image must be exactly what the oracle predicts, otherwise
         * the negative cases below would prove nothing. */
        uint8_t candidate[4];
        memcpy(candidate,expected,4);
        ps5vk_clip_cull_witness_pixel(&probe,witness_case,x,y,EXTENT,candidate);
        if(probe.expected_covered)memcpy(pixel,expected,4);
    }
}

static int classify(unsigned witness_case,const uint8_t *image,
                    struct ps5vk_clip_cull_witness *witness)
{
    memset(witness,0,sizeof(*witness));
    for(unsigned y=0;y<EXTENT;++y)for(unsigned x=0;x<EXTENT;++x)
        ps5vk_clip_cull_witness_pixel(witness,witness_case,x,y,EXTENT,
                                      image+4*((size_t)y*EXTENT+x));
    return ps5vk_clip_cull_witness_verify(witness,witness_case,EXTENT);
}

int main(void)
{
    static uint8_t image[EXTENT*EXTENT*4];
    struct ps5vk_clip_cull_witness witness;
    const uint64_t pixels=(uint64_t)EXTENT*EXTENT;

    /* Every case passes on its own prediction, and the clip cases keep exactly
     * the fraction of the target the plane leaves: half, one quadrant and one
     * quadrant with the cull distances exported alongside. */
    for(unsigned witness_case=0;witness_case<PS5VK_CLIP_CULL_CASES;++witness_case) {
        image_for(witness_case,image);
        assert(classify(witness_case,image,&witness));
        switch(witness_case) {
        case PS5VK_CLIP_CULL_PLAIN:
        case PS5VK_CLIP_CULL_POSITIVE:
        /* A cull distance negative at one vertex only is not a discard. */
        case PS5VK_CLIP_CULL_CULL_HALF:
        /* The pixel read keeps the control's coverage; the distance changes the
         * colour, which the relation below checks against the control image. */
        case PS5VK_CLIP_CULL_PIXEL_READ:
            assert(witness.expected_covered==pixels);
            break;
        case PS5VK_CLIP_CULL_CLIP_HALF:
            assert(witness.expected_covered==pixels/2);
            break;
        case PS5VK_CLIP_CULL_CLIP_QUADRANT:
        case PS5VK_CLIP_CULL_MIXED:
        /* The dynamically indexed write must produce the quadrant's image, so it
         * keeps the same fraction and the same colours. */
        case PS5VK_CLIP_CULL_DYNAMIC_INDEX:
        /* The indirect draw runs the quadrant's program and must render the
         * quadrant's image. */
        case PS5VK_CLIP_CULL_INDIRECT_QUADRANT:
            assert(witness.expected_covered==pixels/4);
            break;
        default:
            assert(witness.expected_covered==0);
            break;
        }
    }

    /* A clipped region that was drawn anyway is not a pass. */
    image_for(PS5VK_CLIP_CULL_CLIP_HALF,image);
    uint8_t expected[4];
    ps5vk_clip_cull_witness_expected(PS5VK_CLIP_CULL_PLAIN,0,0,EXTENT,expected);
    memcpy(image,expected,4); /* column 0 row 0 is inside the cut half */
    assert(!classify(PS5VK_CLIP_CULL_CLIP_HALF,image,&witness));
    assert(witness.foreign==1 && witness.first_foreign_x==0 && witness.first_foreign_y==0);

    /* A covered pixel left at the clear colour, and a covered pixel whose
     * varying is wrong, are both failures. */
    image_for(PS5VK_CLIP_CULL_CLIP_HALF,image);
    memcpy(image+4*((size_t)32*EXTENT+40),ps5vk_clip_cull_clear,4);
    assert(!classify(PS5VK_CLIP_CULL_CLIP_HALF,image,&witness));
    assert(witness.covered==witness.expected_covered-1 && !witness.foreign);
    image_for(PS5VK_CLIP_CULL_CLIP_HALF,image);
    image[4*((size_t)32*EXTENT+40)]=(uint8_t)(image[4*((size_t)32*EXTENT+40)]^0x40u);
    assert(!classify(PS5VK_CLIP_CULL_CLIP_HALF,image,&witness));
    assert(witness.wrong_color==1 && witness.first_wrong_x==40 && witness.first_wrong_y==32);

    /* The cull cases are the ones that distinguish culling from clipping: the
     * mixed one must accept the fully drawn image (it is not a discard), while
     * the ones with a half-space negative at every vertex must refuse it. */
    image_for(PS5VK_CLIP_CULL_PLAIN,image);
    assert(classify(PS5VK_CLIP_CULL_CULL_HALF,image,&witness));
    assert(witness.covered==pixels);
    /* The pixel-read case is judged on the value, not on coverage: the control
     * image covers the same pixels with a plain varying and must fail it, and
     * the read image must fail the control - otherwise the case could pass
     * without the distance ever reaching the pixel stage. */
    {
        static uint8_t read_image[EXTENT*EXTENT*4];
        image_for(PS5VK_CLIP_CULL_PIXEL_READ,read_image);
        assert(classify(PS5VK_CLIP_CULL_PIXEL_READ,read_image,&witness));
        assert(witness.covered==pixels && !witness.wrong_color && !witness.foreign);
        assert(!classify(PS5VK_CLIP_CULL_PIXEL_READ,image,&witness));
        assert(witness.wrong_color>0 && !witness.foreign);
        image_for(PS5VK_CLIP_CULL_PLAIN,image);
        assert(!classify(PS5VK_CLIP_CULL_PLAIN,read_image,&witness));
        assert(witness.wrong_color>0 && !witness.foreign);
        image_for(PS5VK_CLIP_CULL_PLAIN,image);
    }
    assert(!classify(PS5VK_CLIP_CULL_CULL_NEGATIVE,image,&witness));
    assert(witness.foreign>0);
    assert(!classify(PS5VK_CLIP_CULL_CULL_INDEX,image,&witness));
    assert(witness.foreign>0);
    assert(classify(PS5VK_CLIP_CULL_PLAIN,image,&witness));
    assert(witness.covered==pixels);

    /* A target of the wrong size never verifies, and an unknown case is refused
     * instead of being treated as "nothing expected". */
    /* The dynamically indexed case is judged against the statically indexed
     * quadrant: the two images must be identical pixel for pixel, and the
     * dynamic case must refuse the quadrant's own image when the distances it
     * writes are not the ones the case names (the half-clipped image has the
     * right colours on half the target). */
    {
        static uint8_t quadrant_image[EXTENT*EXTENT*4];
        image_for(PS5VK_CLIP_CULL_CLIP_QUADRANT,quadrant_image);
        image_for(PS5VK_CLIP_CULL_DYNAMIC_INDEX,image);
        assert(!memcmp(quadrant_image,image,sizeof(image)));
        assert(classify(PS5VK_CLIP_CULL_DYNAMIC_INDEX,image,&witness));
        image_for(PS5VK_CLIP_CULL_CLIP_HALF,image);
        assert(!classify(PS5VK_CLIP_CULL_DYNAMIC_INDEX,image,&witness));
        assert(witness.foreign>0);
    }
    image_for(PS5VK_CLIP_CULL_PLAIN,image);
    memset(&witness,0,sizeof(witness));
    ps5vk_clip_cull_witness_pixel(&witness,PS5VK_CLIP_CULL_PLAIN,0,0,EXTENT,image);
    assert(!ps5vk_clip_cull_witness_verify(&witness,PS5VK_CLIP_CULL_PLAIN,EXTENT));
    assert(!ps5vk_clip_cull_witness_verify(&witness,PS5VK_CLIP_CULL_CASES,EXTENT));
    assert(!ps5vk_clip_cull_witness_verify(NULL,PS5VK_CLIP_CULL_PLAIN,EXTENT));
    puts("Clip/cull witness: coverage, interpolation and cull-vs-clip distinctions hold");
    return 0;
}

/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host regression for the geometry coverage oracle: every way the native
 * verdict could be fooled is exercised here first, on synthetic images built
 * from the same predicate.
 */
#include "geometry_witness.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { EXTENT = 64 };

static void image_for(unsigned witness_case,uint8_t *image)
{
    for(unsigned y=0;y<EXTENT;++y)for(unsigned x=0;x<EXTENT;++x) {
        uint8_t *pixel=image+4*((size_t)y*EXTENT+x);
        memcpy(pixel,ps5vk_geometry_clear,4);
        struct ps5vk_geometry_witness probe={0};
        uint8_t candidate[4];
        ps5vk_geometry_witness_expected(witness_case,x,y,EXTENT,candidate);
        ps5vk_geometry_witness_pixel(&probe,witness_case,x,y,EXTENT,candidate);
        if(probe.expected_covered)memcpy(pixel,candidate,4);
    }
}

static int classify(unsigned witness_case,const uint8_t *image,
                    struct ps5vk_geometry_witness *witness)
{
    memset(witness,0,sizeof(*witness));
    for(unsigned y=0;y<EXTENT;++y)for(unsigned x=0;x<EXTENT;++x)
        ps5vk_geometry_witness_pixel(witness,witness_case,x,y,EXTENT,
                                     image+4*((size_t)y*EXTENT+x));
    return ps5vk_geometry_witness_verify(witness,witness_case,EXTENT);
}

int main(void)
{
    static uint8_t image[EXTENT*EXTENT*4];
    struct ps5vk_geometry_witness witness;
    const uint64_t pixels=(uint64_t)EXTENT*EXTENT;
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_CONTROL)==-1);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_PASSTHROUGH)==0);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_RECOLOR)==3);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_CASES)==-2);

    for(unsigned witness_case=0;witness_case<PS5VK_GEOMETRY_CASES;++witness_case) {
        image_for(witness_case,image);
        assert(classify(witness_case,image,&witness));
        switch(witness_case) {
        case PS5VK_GEOMETRY_CONTROL:
        case PS5VK_GEOMETRY_PASSTHROUGH:
        case PS5VK_GEOMETRY_RECOLOR:
            assert(witness.expected_covered==pixels);
            break;
        case PS5VK_GEOMETRY_SHRINK:
            /* The scaled triangles tile the centred square: 0.6 of each axis. */
            assert(witness.expected_covered==(uint64_t)(EXTENT*0.6)*(uint64_t)(EXTENT*0.6));
            break;
        default:
            assert(witness.expected_covered==0);
            break;
        }
    }

    /* A suppressed geometry stage that drew anything is a failure, and a
     * passthrough image is not a suppression image. */
    image_for(PS5VK_GEOMETRY_PASSTHROUGH,image);
    assert(!classify(PS5VK_GEOMETRY_SUPPRESS,image,&witness));
    assert(witness.foreign==pixels);
    /* A shrunk image that kept the full coverage is a failure: the union of the
     * scaled triangles must leave the corners clear. */
    assert(!classify(PS5VK_GEOMETRY_SHRINK,image,&witness));
    assert(witness.foreign==pixels-witness.expected_covered);
    /* The varying rewrite changes every colour while keeping the coverage, so
     * the recolor case refuses the passthrough image and vice versa. */
    assert(!classify(PS5VK_GEOMETRY_RECOLOR,image,&witness));
    assert(witness.wrong_color>0);
    image_for(PS5VK_GEOMETRY_RECOLOR,image);
    assert(!classify(PS5VK_GEOMETRY_PASSTHROUGH,image,&witness));
    assert(witness.wrong_color>0);
    assert(classify(PS5VK_GEOMETRY_RECOLOR,image,&witness));
    assert(witness.covered==pixels);
    /* A covered pixel left at the clear colour, and a covered pixel whose
     * varying is wrong, are both failures. */
    image_for(PS5VK_GEOMETRY_PASSTHROUGH,image);
    memcpy(image+4*((size_t)10*EXTENT+10),ps5vk_geometry_clear,4);
    assert(!classify(PS5VK_GEOMETRY_PASSTHROUGH,image,&witness));
    assert(witness.covered==witness.expected_covered-1 && !witness.foreign);
    image_for(PS5VK_GEOMETRY_PASSTHROUGH,image);
    image[4*((size_t)10*EXTENT+10)]=(uint8_t)(image[4*((size_t)10*EXTENT+10)]^0x40u);
    assert(!classify(PS5VK_GEOMETRY_PASSTHROUGH,image,&witness));
    assert(witness.wrong_color==1 && witness.first_wrong_x==10 && witness.first_wrong_y==10);
    /* A target of the wrong size or an unknown case never verifies. */
    memset(&witness,0,sizeof(witness));
    ps5vk_geometry_witness_pixel(&witness,PS5VK_GEOMETRY_CONTROL,0,0,EXTENT,image);
    assert(!ps5vk_geometry_witness_verify(&witness,PS5VK_GEOMETRY_CONTROL,EXTENT));
    assert(!ps5vk_geometry_witness_verify(&witness,PS5VK_GEOMETRY_CASES,EXTENT));
    assert(!ps5vk_geometry_witness_verify(NULL,PS5VK_GEOMETRY_CONTROL,EXTENT));
    puts("Geometry witness: passthrough, shrink, suppression and varying rewrite hold");
    return 0;
}

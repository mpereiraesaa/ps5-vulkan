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
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_AMPLIFY)==4);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_CONSTANT)==5);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_POSITIONS)==6);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_SENTINEL)==10);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_CASES)==-2);
    /* Every declared case must be registered: an unregistered one would be
     * skipped by the native matrix without any run logging that it was missing. */
    for(unsigned witness_case=0;witness_case<PS5VK_GEOMETRY_CASES;++witness_case)
        assert(ps5vk_geometry_witness_mode(witness_case)!=-2);

    for(unsigned witness_case=0;witness_case<PS5VK_GEOMETRY_CASES;++witness_case) {
        image_for(witness_case,image);
        assert(classify(witness_case,image,&witness));
        switch(witness_case) {
        case PS5VK_GEOMETRY_CONTROL:
        case PS5VK_GEOMETRY_PASSTHROUGH:
        case PS5VK_GEOMETRY_RECOLOR:
        case PS5VK_GEOMETRY_AMPLIFY:
        case PS5VK_GEOMETRY_POSITIONS:
        case PS5VK_GEOMETRY_SENTINEL:
            assert(witness.expected_covered==pixels);
            break;
        case PS5VK_GEOMETRY_SHRINK:
            /* The scaled triangles tile the centred square: 0.6 of each axis. */
            assert(witness.expected_covered==(uint64_t)(EXTENT*0.6)*(uint64_t)(EXTENT*0.6));
            break;
        case PS5VK_GEOMETRY_CONSTANT:
            /* The fixed quad is half the target on each axis. */
            assert(witness.expected_covered==(uint64_t)(EXTENT/2)*(uint64_t)(EXTENT/2));
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
    /* The amplified image must be the passthrough image: a sub-triangle that
     * left a gap shows up as a missing pixel. */
    image_for(PS5VK_GEOMETRY_AMPLIFY,image);
    assert(classify(PS5VK_GEOMETRY_AMPLIFY,image,&witness));
    assert(witness.covered==pixels);
    memcpy(image+4*((size_t)5*EXTENT+5),ps5vk_geometry_clear,4);
    assert(!classify(PS5VK_GEOMETRY_AMPLIFY,image,&witness));
    assert(witness.covered==witness.expected_covered-1);
    /* The sentinel exists so that a geometry stage's read is judged by the value
     * it produced, not only by the coverage: its mapping must be its own, and it
     * must refuse both the control's colours and the empty image a zero read
     * would leave behind. */
    image_for(PS5VK_GEOMETRY_SENTINEL,image);
    assert(classify(PS5VK_GEOMETRY_SENTINEL,image,&witness));
    assert(witness.covered==pixels);
    assert(!classify(PS5VK_GEOMETRY_CONTROL,image,&witness));
    assert(witness.wrong_color==pixels);
    image_for(PS5VK_GEOMETRY_CONTROL,image);
    assert(!classify(PS5VK_GEOMETRY_SENTINEL,image,&witness));
    assert(witness.wrong_color==pixels);
    for(size_t i=0;i<sizeof(image);i+=4)memcpy(image+i,ps5vk_geometry_clear,4);
    assert(!classify(PS5VK_GEOMETRY_SENTINEL,image,&witness));
    assert(witness.wrong_color==pixels);
    uint8_t sentinel_pixel[4],control_pixel[4];
    ps5vk_geometry_witness_expected(PS5VK_GEOMETRY_SENTINEL,7,9,EXTENT,sentinel_pixel);
    ps5vk_geometry_witness_expected(PS5VK_GEOMETRY_CONTROL,7,9,EXTENT,control_pixel);
    assert(sentinel_pixel[0]==control_pixel[0] && sentinel_pixel[1]==control_pixel[1]);
    assert(sentinel_pixel[2]!=control_pixel[2]);
    /* A target of the wrong size or an unknown case never verifies. */
    memset(&witness,0,sizeof(witness));
    ps5vk_geometry_witness_pixel(&witness,PS5VK_GEOMETRY_CONTROL,0,0,EXTENT,image);
    assert(!ps5vk_geometry_witness_verify(&witness,PS5VK_GEOMETRY_CONTROL,EXTENT));
    assert(!ps5vk_geometry_witness_verify(&witness,PS5VK_GEOMETRY_CASES,EXTENT));
    assert(!ps5vk_geometry_witness_verify(NULL,PS5VK_GEOMETRY_CONTROL,EXTENT));
    puts("Geometry witness: passthrough, shrink, suppression, varying rewrite and sentinel value hold");
    return 0;
}

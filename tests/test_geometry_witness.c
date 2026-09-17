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
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_INDEXED_MARKER)==11);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_READ_V0)==12);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_READ_V1)==13);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_READ_V2)==14);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_ENVELOPE)==15);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_INVOCATIONS)==16);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_COMPONENTS)==17);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_PRIMITIVE_ID)==18);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_POINTS)==19);
    assert(ps5vk_geometry_witness_mode(PS5VK_GEOMETRY_LINES)==20);
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
        case PS5VK_GEOMETRY_INDEXED_MARKER:
            /* Two 12x12 markers at 64x64: one per input primitive, at the place
             * the read position puts them. The exact count matters - a marker
             * that moved or went missing changes it. */
            assert(witness.expected_covered==288u);
            break;
        case PS5VK_GEOMETRY_READ_V0:
        case PS5VK_GEOMETRY_READ_V1:
        case PS5VK_GEOMETRY_READ_V2:
            /* Seven primitives, each writing two quadrants at the place its own
             * read value puts them: fourteen small squares from one value per
             * item. The exact count matters - a read that returned another item
             * moves its quadrant to that item's place and changes its colour. */
            assert(witness.expected_covered==336u);
            assert(witness.covered==336u);
            break;
        case PS5VK_GEOMETRY_PRIMITIVE_ID:
            /* Two columns, one per primitive of the witness draw, each as wide as
             * its marker: 36 px at 64x64. A constant id collapses them to one. */
            assert(witness.expected_covered==36u);
            assert(witness.covered==36u);
            break;
        case PS5VK_GEOMETRY_COMPONENTS:
            /* The centred quad, the same shape the constant case draws: 1024 px
             * at 64x64 with the colour the 64 input components produce. */
            assert(witness.expected_covered==1024u);
            assert(witness.covered==1024u);
            break;
        case PS5VK_GEOMETRY_INVOCATIONS:
            /* 32 columns, one per invocation, each as wide as its marker: 480 px
             * at 64x64. A stage that ran once covers one column. */
            assert(witness.expected_covered==480u);
            assert(witness.covered==480u);
            break;
        case PS5VK_GEOMETRY_ENVELOPE:
            /* The 256-vertex ribbon tiles a band 1.5 wide and 0.5 tall: 48x16
             * pixels at 64x64. A stage that emitted fewer vertices covers less,
             * so the exact count is the measurement. */
            assert(witness.expected_covered==768u);
            assert(witness.covered==768u);
            break;
        case PS5VK_GEOMETRY_POINTS:
            /* Four markers, one per input point, each 0.24 NDC on a side: 218 px
             * at 64x64. A stage that read one fixed item would put every marker
             * at that item's place, which changes the count and the colours. */
            assert(witness.expected_covered==218u);
            assert(witness.covered==218u);
            break;
        case PS5VK_GEOMETRY_LINES:
            /* Three markers, one per input segment, each as wide as the segment
             * it was built from plus a fixed margin: 467 px at 64x64. A stage
             * that read only gl_in[0] would emit half-length markers. */
            assert(witness.expected_covered==467u);
            assert(witness.covered==467u);
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
    /* The marker diagnostic is judged on place AND colour, so it refuses the
     * full-image cases (its own image is two small quads), and an image with a
     * marker missing is a failure rather than a "coverage" answer. */
    assert(!classify(PS5VK_GEOMETRY_INDEXED_MARKER,image,&witness));
    image_for(PS5VK_GEOMETRY_INDEXED_MARKER,image);
    assert(classify(PS5VK_GEOMETRY_INDEXED_MARKER,image,&witness));
    assert(witness.covered==288u && !witness.foreign && !witness.wrong_color);
    assert(!classify(PS5VK_GEOMETRY_CONTROL,image,&witness));
    {
        static uint8_t moved[EXTENT*EXTENT*4];
        memcpy(moved,image,sizeof(moved));
        for(unsigned y=0;y<EXTENT;++y)for(unsigned x=0;x<EXTENT;++x) {
            uint8_t *pixel=moved+4*((size_t)y*EXTENT+x);
            memcpy(pixel,ps5vk_geometry_clear,4);
            struct ps5vk_geometry_witness probe={0};
            uint8_t candidate[4];
            ps5vk_geometry_witness_expected(PS5VK_GEOMETRY_INDEXED_MARKER,x,y,EXTENT,candidate);
            ps5vk_geometry_witness_pixel(&probe,PS5VK_GEOMETRY_INDEXED_MARKER,x,y,EXTENT,candidate);
            /* Keep only the right-hand marker; drop the left one entirely. */
            if(probe.expected_covered && candidate[0]>128u)memcpy(pixel,candidate,4);
        }
        assert(!classify(PS5VK_GEOMETRY_INDEXED_MARKER,moved,&witness));
        /* The dropped marker's 144 pixels are expected and left clear, so the
         * oracle reports exactly those 144 as the wrong colour (the clear value
         * is not the marker's own colour): a partial image can never satisfy
         * the case, and the verdict says how many pixels went missing. */
        assert(witness.expected_covered==288u && witness.covered==144u &&
               witness.wrong_color==144u && !witness.foreign);
    }
    /* The readback is judged on the exact 32 bits of the value read, so a
     * quadrant that carries the same shape with the bytes of a DIFFERENT value
     * is a wrong-colour failure - that is the whole point of the case - and the
     * marker's image (same kind of shape, other colours and places) cannot pass
     * for it either. */
    image_for(PS5VK_GEOMETRY_READ_V0,image);
    assert(classify(PS5VK_GEOMETRY_READ_V0,image,&witness));
    assert(witness.covered==336u && !witness.foreign && !witness.wrong_color);
    {
        static uint8_t other[EXTENT*EXTENT*4];
        memcpy(other,image,sizeof(other));
        /* Both high-byte quadrants carry a different value's top byte: a read
         * that returned a float of the same magnitude but another sign or
         * exponent makes exactly this image, and it must fail. */
        for(unsigned y=0;y<EXTENT;++y)for(unsigned x=0;x<EXTENT;++x) {
            struct ps5vk_geometry_witness probe={0};
            uint8_t candidate[4];
            ps5vk_geometry_witness_expected(PS5VK_GEOMETRY_READ_V0,x,y,EXTENT,candidate);
            ps5vk_geometry_witness_pixel(&probe,PS5VK_GEOMETRY_READ_V0,x,y,EXTENT,candidate);
            if(probe.expected_covered && candidate[1]==128u && candidate[2]==64u)
                other[4*((size_t)y*EXTENT+x)+0]=(uint8_t)(candidate[0]^0x40u);
        }
        assert(!classify(PS5VK_GEOMETRY_READ_V0,other,&witness));
        assert(witness.wrong_color==24u*7u && !witness.foreign);
    }
    image_for(PS5VK_GEOMETRY_INDEXED_MARKER,image);
    assert(!classify(PS5VK_GEOMETRY_READ_V0,image,&witness));
    assert(witness.foreign>0u);
    /* The three readbacks read three different vertices, so the V1 and V2
     * images (one column each, opposite signs) must refuse each other and the
     * V0 image (both columns), and an image whose high-byte quadrant was left
     * clear must not pass for any of them. */
    image_for(PS5VK_GEOMETRY_READ_V1,image);
    assert(classify(PS5VK_GEOMETRY_READ_V1,image,&witness));
    assert(witness.covered==336u);
    assert(!classify(PS5VK_GEOMETRY_READ_V2,image,&witness));
    assert(!classify(PS5VK_GEOMETRY_READ_V0,image,&witness));
    image_for(PS5VK_GEOMETRY_READ_V2,image);
    assert(classify(PS5VK_GEOMETRY_READ_V2,image,&witness));
    assert(witness.covered==336u);
    assert(!classify(PS5VK_GEOMETRY_READ_V1,image,&witness));
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
    /* The input families are judged on place AND colour per item, so each one
     * refuses the other family's image and the full-coverage images, and an
     * image where one marker carries a different item's colour can never pass -
     * that is the whole point of giving every input item its own colour. */
    image_for(PS5VK_GEOMETRY_POINTS,image);
    assert(classify(PS5VK_GEOMETRY_POINTS,image,&witness));
    assert(witness.covered==218u && !witness.foreign && !witness.wrong_color);
    assert(!classify(PS5VK_GEOMETRY_LINES,image,&witness));
    assert(witness.foreign>0u || witness.wrong_color>0u);
    image_for(PS5VK_GEOMETRY_LINES,image);
    assert(classify(PS5VK_GEOMETRY_LINES,image,&witness));
    assert(witness.covered==467u && !witness.foreign && !witness.wrong_color);
    assert(!classify(PS5VK_GEOMETRY_POINTS,image,&witness));
    assert(witness.foreign>0u || witness.wrong_color>0u);
    /* A point marker that carries another point's colour is refused: the read
     * returned an item, just not this one. */
    image_for(PS5VK_GEOMETRY_POINTS,image);
    {
        for(unsigned y=0;y<EXTENT;++y)for(unsigned x=0;x<EXTENT;++x) {
            /* A FRESH probe per pixel: the counters are cumulative, so reusing
             * one would turn "this pixel is covered" into "some pixel was". */
            struct ps5vk_geometry_witness probe={0};
            uint8_t candidate[4];
            ps5vk_geometry_witness_expected(PS5VK_GEOMETRY_POINTS,x,y,EXTENT,candidate);
            ps5vk_geometry_witness_pixel(&probe,PS5VK_GEOMETRY_POINTS,x,y,EXTENT,candidate);
            /* Repaint every covered pixel with the LAST point's colour: the
             * marker places are right, the colours name the wrong item. */
            if(probe.expected_covered)
                memset(image+4*((size_t)y*EXTENT+x),255,4);
        }
        assert(!classify(PS5VK_GEOMETRY_POINTS,image,&witness));
        assert(witness.wrong_color>0u && !witness.foreign);
    }
    /* A missing line marker is a failure even though the other two are intact,
     * and a marker of the wrong size (a stage that read only gl_in[0]) lands
     * outside the oracle's rectangle, which the foreign count reports. */
    image_for(PS5VK_GEOMETRY_LINES,image);
    for(size_t i=0;i<sizeof(image);i+=4) {
        const size_t pixel=i/4;
        if(pixel%EXTENT==30u)memcpy(image+i,ps5vk_geometry_clear,4);
    }
    assert(!classify(PS5VK_GEOMETRY_LINES,image,&witness));
    assert(witness.wrong_color>0u);
    /* A target of the wrong size or an unknown case never verifies. */
    memset(&witness,0,sizeof(witness));
    ps5vk_geometry_witness_pixel(&witness,PS5VK_GEOMETRY_CONTROL,0,0,EXTENT,image);
    assert(!ps5vk_geometry_witness_verify(&witness,PS5VK_GEOMETRY_CONTROL,EXTENT));
    assert(!ps5vk_geometry_witness_verify(&witness,PS5VK_GEOMETRY_CASES,EXTENT));
    assert(!ps5vk_geometry_witness_verify(NULL,PS5VK_GEOMETRY_CONTROL,EXTENT));
    puts("Geometry witness: passthrough, shrink, suppression, varying rewrite, sentinel value "
         "and raw read bytes hold, and the 256-vertex envelope band verifies");
    return 0;
}

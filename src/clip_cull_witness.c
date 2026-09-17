/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "clip_cull_witness.h"

/* The witness render pass clears to opaque black; the vertex stage writes any
 * covered pixel with a non-zero red component for every pixel centre of a
 * target with an extent of at least two, so "cleared" and "covered" can never
 * be confused by a colour that happens to be black. */
const uint8_t ps5vk_clip_cull_clear[4]={0u,0u,0u,255u};

static uint8_t unorm8(double value)
{
    if(value<=0.0)return 0u;
    if(value>=1.0)return 255u;
    return (uint8_t)(value*255.0+0.5);
}

static int covers(unsigned witness_case,double ndc_x,double ndc_y)
{
    switch(witness_case) {
    case PS5VK_CLIP_CULL_PLAIN:
    case PS5VK_CLIP_CULL_POSITIVE:
    /* A cull distance negative at one vertex only is not a discard: the rule
     * needs one half-space negative for every vertex of the primitive, and this
     * witness draws one primitive whose x values have both signs. */
    case PS5VK_CLIP_CULL_CULL_HALF:
        return 1;
    case PS5VK_CLIP_CULL_CLIP_HALF:
        return ndc_x>=0.0;
    case PS5VK_CLIP_CULL_CLIP_QUADRANT:
    case PS5VK_CLIP_CULL_MIXED:
    /* The dynamically indexed write must produce the quadrant's image. */
    case PS5VK_CLIP_CULL_DYNAMIC_INDEX:
        return ndc_x>=0.0 && ndc_y>=0.0;
    default:
        /* Both remaining cull cases have one distance index negative for every
         * vertex, so the primitive is discarded. */
        return 0;
    }
}

void ps5vk_clip_cull_witness_expected(unsigned x,unsigned y,unsigned extent,uint8_t rgba[4])
{
    /* Framebuffer pixel centres in the coordinates the witness vertex stage
     * interpolates: u,v are the vertex-stage varying, and the NDC values the
     * distance planes are built from are the same numbers scaled to -1..1.
     *
     * The readback's first row is the witness vertex stage's NDC y = -1, which
     * the native run measured before this oracle was allowed to judge a case
     * (row 16 of 64 interpolated the varying to 0.26, not 0.74). The oracle
     * and the distance predicates share this convention, so a flipped or
     * swapped varying cannot pass. */
    const double u=((double)x+0.5)/(double)extent;
    const double v=((double)y+0.5)/(double)extent;
    rgba[0]=unorm8(u);
    rgba[1]=unorm8(v);
    rgba[2]=unorm8(0.5);
    rgba[3]=255u;
}

void ps5vk_clip_cull_witness_pixel(struct ps5vk_clip_cull_witness *witness,
    unsigned witness_case,unsigned x,unsigned y,unsigned extent,const uint8_t rgba[4])
{
    if(!witness || witness_case>=PS5VK_CLIP_CULL_CASES || !rgba || extent<2u)return;
    ++witness->pixels;
    const double u=((double)x+0.5)/(double)extent;
    const double v=((double)y+0.5)/(double)extent;
    if(!covers(witness_case,2.0*u-1.0,2.0*v-1.0)) {
        if(rgba[0]!=ps5vk_clip_cull_clear[0] || rgba[1]!=ps5vk_clip_cull_clear[1] ||
           rgba[2]!=ps5vk_clip_cull_clear[2] || rgba[3]!=ps5vk_clip_cull_clear[3]) {
            if(!witness->foreign) {
                for(unsigned i=0;i<4;++i)witness->first_foreign[i]=rgba[i];
                witness->first_foreign_x=x;witness->first_foreign_y=y;
            }
            ++witness->foreign;
        }
        return;
    }
    ++witness->expected_covered;
    uint8_t want[4];
    ps5vk_clip_cull_witness_expected(x,y,extent,want);
    int matches=1;
    /* One unorm step of slack: the varying is affine, so only the conversion of
     * the exact value may round, and never by more than that. */
    for(unsigned i=0;i<4;++i)if((unsigned)rgba[i]>want[i]+1u || want[i]>rgba[i]+1u)matches=0;
    if(!matches) {
        if(!witness->wrong_color) {
            for(unsigned i=0;i<4;++i)witness->first_wrong[i]=rgba[i];
            witness->first_wrong_x=x;witness->first_wrong_y=y;
        }
        ++witness->wrong_color;
        return;
    }
    ++witness->covered;
}

int ps5vk_clip_cull_witness_verify(const struct ps5vk_clip_cull_witness *witness,
    unsigned witness_case,unsigned extent)
{
    if(!witness || witness_case>=PS5VK_CLIP_CULL_CASES || extent<2u)return 0;
    const uint64_t pixels=(uint64_t)extent*extent;
    if(witness->pixels!=pixels)return 0;
    if(witness->covered!=witness->expected_covered)return 0;
    if(witness->foreign || witness->wrong_color)return 0;
    /* The case's own predicate has to be the shape the case is named after, so
     * the verdict cannot come from an oracle that expected nothing to happen. */
    switch(witness_case) {
    case PS5VK_CLIP_CULL_PLAIN:
    case PS5VK_CLIP_CULL_POSITIVE:
    case PS5VK_CLIP_CULL_CULL_HALF:
        return witness->expected_covered==pixels;
    case PS5VK_CLIP_CULL_CULL_NEGATIVE:
    case PS5VK_CLIP_CULL_CULL_INDEX:
        return witness->expected_covered==0u && witness->covered==0u;
    case PS5VK_CLIP_CULL_CLIP_HALF:
    case PS5VK_CLIP_CULL_CLIP_QUADRANT:
    case PS5VK_CLIP_CULL_MIXED:
    case PS5VK_CLIP_CULL_DYNAMIC_INDEX:
        return witness->expected_covered>0u && witness->expected_covered<pixels;
    }
    return 0;
}

/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "geometry_witness.h"

const uint8_t ps5vk_geometry_clear[4]={0u,0u,0u,255u};

/* The geometry stage scales the vertices of the two triangles that tile the
 * target, so the covered square has this half extent in NDC. */
static const double shrink_extent=0.6;

int ps5vk_geometry_witness_mode(unsigned witness_case)
{
    switch(witness_case) {
    case PS5VK_GEOMETRY_CONTROL:return -1;
    case PS5VK_GEOMETRY_PASSTHROUGH:return 0;
    case PS5VK_GEOMETRY_SHRINK:return 1;
    case PS5VK_GEOMETRY_SUPPRESS:return 2;
    case PS5VK_GEOMETRY_RECOLOR:return 3;
    case PS5VK_GEOMETRY_AMPLIFY:return 4;
    case PS5VK_GEOMETRY_CONSTANT:return 5;
    case PS5VK_GEOMETRY_POSITIONS:return 6;
    case PS5VK_GEOMETRY_SENTINEL:return 10;
    case PS5VK_GEOMETRY_INDEXED_MARKER:return 11;
    }
    return -2;
}

/* The indexed-marker diagnostic's geometry, mirrored from the witness geometry
 * stage's MODE 11 so the oracle and the shader cannot drift apart: the read
 * position is clamped into the target, a small triangle is emitted around it,
 * and the colour is the UNCLAMPED read position mapped to 0..1. */
static void marker_centre(int marker,double *x,double *y)
{
    /* The witness draws six vertices in two triangles; gl_in[0] of each is the
     * first vertex of that triangle. */
    const double px=marker?-1.2:1.2;
    const double py=-1.2;
    *x=px<-0.75?-0.75:(px>0.75?0.75:px);
    *y=py<-0.75?-0.75:(py>0.75?0.75:py);
}

static void marker_colour(int marker,double *r,double *g)
{
    const double px=marker?1.2:-1.2;
    const double py=-1.2;
    *r=px*0.5+0.5;
    *g=py*0.5+0.5;
}

static int marker_covers(int marker,double ndc_x,double ndc_y)
{
    double cx=0.0,cy=0.0;
    marker_centre(marker,&cx,&cy);
    /* The shader emits an axis-aligned quad, so the oracle's test is a rectangle
     * and cannot disagree with the rasterizer's pixel-centre rule. */
    return ndc_x>=cx-0.2 && ndc_x<=cx+0.2 && ndc_y>=cy-0.2 && ndc_y<=cy+0.2;
}

static uint8_t unorm8(double value)
{
    if(value<=0.0)return 0u;
    if(value>=1.0)return 255u;
    return (uint8_t)(value*255.0+0.5);
}

static int covers(unsigned witness_case,double ndc_x,double ndc_y)
{
    switch(witness_case) {
    case PS5VK_GEOMETRY_CONTROL:
    case PS5VK_GEOMETRY_PASSTHROUGH:
    case PS5VK_GEOMETRY_RECOLOR:
    /* The three sub-triangles tile the input triangle, so the coverage of the
     * amplified image is the control's coverage: an amplification that left a
     * gap or an overlap would show up as a missing or foreign pixel. */
    case PS5VK_GEOMETRY_AMPLIFY:
    /* The input positions must trace the same triangles the vertex stage drew. */
    case PS5VK_GEOMETRY_POSITIONS:
    /* Sentinel: the input triangle is emitted unchanged, so the coverage is the
     * control's and the VALUE assertion comes from the expected-colour branch
     * below. Without this case the oracle would expect an empty image and a
     * correct draw would read as foreign pixels. */
    case PS5VK_GEOMETRY_SENTINEL:
        return 1;
    case PS5VK_GEOMETRY_SHRINK:
        return ndc_x>=-shrink_extent && ndc_x<=shrink_extent &&
               ndc_y>=-shrink_extent && ndc_y<=shrink_extent;
    case PS5VK_GEOMETRY_CONSTANT:
        /* The fixed quad the stage emits without reading its input. */
        return ndc_x>=-0.5 && ndc_x<=0.5 && ndc_y>=-0.5 && ndc_y<=0.5;
    /* The indexed-marker diagnostic covers two small triangles, one per input
     * primitive, at the place the read position puts them. */
    case PS5VK_GEOMETRY_INDEXED_MARKER:
        return marker_covers(0,ndc_x,ndc_y) || marker_covers(1,ndc_x,ndc_y);
    default:
        /* The stage emits nothing at all. */
        return 0;
    }
}

void ps5vk_geometry_witness_expected(unsigned witness_case,unsigned x,unsigned y,
    unsigned extent,uint8_t rgba[4])
{
    /* Framebuffer pixel centres in the coordinates the witness vertex stage
     * interpolates. The readback's first row is NDC y = -1, the convention the
     * clip/cull witness measured on this path. */
    const double u=((double)x+0.5)/(double)extent;
    const double v=((double)y+0.5)/(double)extent;
    if(witness_case==PS5VK_GEOMETRY_POSITIONS) {
        rgba[0]=rgba[1]=rgba[2]=255u;
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_CONSTANT) {
        rgba[0]=unorm8(0.25);
        rgba[1]=unorm8(0.5);
        rgba[2]=unorm8(0.75);
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_SENTINEL) {
        /* The geometry stage builds this colour from the position it READ from
         * gl_in, not from the varying: the vertex stage's own affine mapping
         * ((p+1)*0.5) with a distinct blue. Barycentric interpolation of an
         * affine varying reproduces that function at the pixel centre, so a
         * correct read must put exactly (u,v) here with blue 0.25 - different
         * from the control's 0.5, so this branch cannot be satisfied by the
         * control image. A read that returned zeros collapses the triangle (no
         * coverage; the expected_covered==pixels verdict fails) and a read that
         * returned another item's data moves or reshapes it (foreign pixels).
         *
         * Limit, stated rather than hidden: because the colour is affine in the
         * read position and the input is two structurally identical triangles
         * (B==D and C==F) each processed with gl_in.length()==3, exchanging the
         * two primitives' items wholesale maps the image onto itself. The
         * sentinel separates a correct read from a zero read, a garbage read and
         * a shifted item; it does not separate a full exchange of the two
         * triangles, which carries no observable difference on this input. */
        rgba[0]=unorm8(u);
        rgba[1]=unorm8(v);
        rgba[2]=unorm8(0.25);
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_INDEXED_MARKER) {
        /* Each marker carries the colour of the positions it was built from, so
         * the two input primitives produce two distinguishable markers: red 0
         * for the triangle whose first vertex is (-1.2,-1.2), red 255 for the
         * one starting at (1.2,-1.2). A read that returned a different vertex
         * changes both the place and the colour, and the marker that would have
         * been there is missing. */
        double r=0.0,g=0.0;
        marker_colour(marker_covers(1,2.0*u-1.0,2.0*v-1.0)?1:0,&r,&g);
        rgba[0]=unorm8(r);
        rgba[1]=unorm8(g);
        rgba[2]=unorm8(0.25);
        rgba[3]=255u;
        return;
    }
    const double red=witness_case==PS5VK_GEOMETRY_RECOLOR?1.0-u:u;
    rgba[0]=unorm8(red);
    rgba[1]=unorm8(v);
    rgba[2]=unorm8(0.5);
    rgba[3]=255u;
}

void ps5vk_geometry_witness_pixel(struct ps5vk_geometry_witness *witness,
    unsigned witness_case,unsigned x,unsigned y,unsigned extent,const uint8_t rgba[4])
{
    if(!witness || witness_case>=PS5VK_GEOMETRY_CASES || !rgba || extent<2u)return;
    ++witness->pixels;
    const double u=((double)x+0.5)/(double)extent;
    const double v=((double)y+0.5)/(double)extent;
    if(!covers(witness_case,2.0*u-1.0,2.0*v-1.0)) {
        if(rgba[0]!=ps5vk_geometry_clear[0] || rgba[1]!=ps5vk_geometry_clear[1] ||
           rgba[2]!=ps5vk_geometry_clear[2] || rgba[3]!=ps5vk_geometry_clear[3]) {
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
    ps5vk_geometry_witness_expected(witness_case,x,y,extent,want);
    /* One unorm step of slack: only the conversion of the exact value rounds. */
    int matches=1;
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

int ps5vk_geometry_witness_verify(const struct ps5vk_geometry_witness *witness,
    unsigned witness_case,unsigned extent)
{
    if(!witness || witness_case>=PS5VK_GEOMETRY_CASES || extent<2u)return 0;
    const uint64_t pixels=(uint64_t)extent*extent;
    if(witness->pixels!=pixels)return 0;
    if(witness->covered!=witness->expected_covered)return 0;
    if(witness->foreign || witness->wrong_color)return 0;
    /* The case's predicate has to be the shape the case is named after, so the
     * verdict cannot come from an oracle that expected nothing to happen. */
    switch(witness_case) {
    case PS5VK_GEOMETRY_CONTROL:
    case PS5VK_GEOMETRY_PASSTHROUGH:
    case PS5VK_GEOMETRY_RECOLOR:
    case PS5VK_GEOMETRY_AMPLIFY:
    case PS5VK_GEOMETRY_POSITIONS:
    /* Sentinel: the input triangle unchanged, so the coverage is the control's,
     * and the colour assertion is the expected-colour branch for this case. Both
     * halves of the verdict are real: a pass means the value the geometry stage
     * read produced the exact per-pixel colour the oracle predicted. */
    case PS5VK_GEOMETRY_SENTINEL:
        return witness->expected_covered==pixels;
    case PS5VK_GEOMETRY_SHRINK:
    case PS5VK_GEOMETRY_CONSTANT:
        return witness->expected_covered>0u && witness->expected_covered<pixels;
    /* The two markers cover a small part of the target: real coverage, not the
     * whole image, and not the empty image a stage that never ran produces. */
    case PS5VK_GEOMETRY_INDEXED_MARKER:
        return witness->expected_covered>0u && witness->expected_covered<pixels;
    case PS5VK_GEOMETRY_SUPPRESS:
        return witness->expected_covered==0u && witness->covered==0u;
    }
    return 0;
}

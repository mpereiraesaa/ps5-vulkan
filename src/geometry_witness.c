/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "geometry_witness.h"
#include <string.h>

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
    case PS5VK_GEOMETRY_READ_V0:return 12;
    case PS5VK_GEOMETRY_READ_V1:return 13;
    case PS5VK_GEOMETRY_READ_V2:return 14;
    case PS5VK_GEOMETRY_ENVELOPE:return 15;
    case PS5VK_GEOMETRY_INVOCATIONS:return 16;
    case PS5VK_GEOMETRY_COMPONENTS:return 17;
    case PS5VK_GEOMETRY_PRIMITIVE_ID:return 18;
    case PS5VK_GEOMETRY_POINTS:return 19;
    case PS5VK_GEOMETRY_LINES:return 20;
    }
    return -2;
}

/* The point/line families' shared input, mirroring the pre-raster module
 * experiments/graphics/runtime_geometry_family.vert exactly: vertex index i
 * carries this position and the colour (i/5, 0.5, 0.25). The point case draws
 * the first four as a point list, the line case all six as three segments, so
 * the markers of one case never touch each other and every marker's place and
 * colour is a function of the item the stage read. */
static const double family_positions[PS5VK_GEOMETRY_FAMILY_VERTICES][2]={
    {-0.6,-0.5},{-0.2,-0.5},{0.2,0.3},{0.7,0.3},{-0.7,0.6},{0.1,0.6}};
void ps5vk_geometry_witness_family_vertex(unsigned index,double position[2],
    double colour[3])
{
    if(index>=PS5VK_GEOMETRY_FAMILY_VERTICES)index=0u;
    position[0]=family_positions[index][0];
    position[1]=family_positions[index][1];
    colour[0]=(double)index/5.0;
    colour[1]=0.5;
    colour[2]=0.25;
}
/* The point counts of the two input families, and the marker geometry the
 * stages emit. Both are module constants: the point module declares a
 * triangle_strip of four vertices per input point, the line module the same per
 * input segment. */
enum { FAMILY_POINTS = 4 };
enum { FAMILY_SEGMENTS = 3 };
/* The point module's half extent, both axes (runtime_geometry_points.geom). */
static const double family_point_extent=0.12;

/* The readback's own input: a diagnostic-only pre-raster half gives vertex i the
 * position x = -0.6875 + 0.0625 * i. Both constants are exact in binary, so the
 * value is exactly representable and unique per index, and the bytes the
 * geometry half reads NAME the item they came from - which is what turns "the
 * read returned something else" into "the read returned vertex 15". The draw is
 * 21 vertices = 7 triangles, so item indices 0..20 exist, which is enough to
 * name an item-unit, a dword-scaled and a byte-scaled read differently. */
enum { READ_PRIMITIVES = 7 };
enum { READ_CASES = 3 };
/* The invocations the pipeline declares: the feature's mandatory minimum. */
enum { INVOCATION_COUNT = 32 };
/* The witness draw is two triangles, so the primitive ids are zero and one. */
enum { PRIMITIVE_COUNT = 2 };

static float read_bytes_vertex_x(unsigned vertex)
{ return -0.6875f + 0.0625f * (float)vertex; }

/* Which of the three readback cases this is, or -1. */
static int read_bytes_index(unsigned witness_case)
{
    if(witness_case<PS5VK_GEOMETRY_READ_V0 ||
       witness_case>PS5VK_GEOMETRY_READ_V2)return -1;
    return (int)(witness_case-PS5VK_GEOMETRY_READ_V0);
}

/* The value the readback of `index` must produce for primitive p: gl_in[index]
 * of a triangle list is the vertex 3p+index. */
static float read_bytes_expected(unsigned index,unsigned primitive)
{ return read_bytes_vertex_x(3u*primitive+index); }
/* The two quadrants each read is written into: the low three bytes below, the
 * top byte above, both at the place the value read puts them. The quadrants are
 * small because the readback's input gives every item its own value, and those
 * places must not overlap each other. */
static const double read_bytes_rows[2]={-0.5,0.5};
/* Narrow in x so that neighbouring items' places cannot touch: the readback's
 * values are 0.0625 apart and this is 0.03 either side. */
static const double read_bytes_half_extent=0.03;

/* Where a read value is written: the value itself, clamped into the target, so
 * a read that returned another item lands at that item's own place carrying
 * that item's own bytes and the image names which item was read. */
static double read_bytes_column(double value)
{ return value<-0.75?-0.75:(value>0.75?0.75:value); }

/* One readback quadrant: an axis-aligned square, so the oracle's per-pixel test
 * and the rasterizer's pixel-centre rule agree exactly. */
static int read_bytes_covers(double value,double row,double ndc_x,double ndc_y)
{
    const double cx=read_bytes_column(value);
    return ndc_x>=cx-read_bytes_half_extent && ndc_x<=cx+read_bytes_half_extent &&
           ndc_y>=row-0.2 && ndc_y<=row+0.2;
}

/* The bytes of the value the way the geometry stage writes them: the low three
 * bytes in the lower quadrant's colour, the top byte in the upper one's red. */
static void read_bytes_colour(double value,double row,uint8_t rgba[4])
{
    const float narrowed=(float)value;
    uint32_t bits=0;
    memcpy(&bits,&narrowed,sizeof(bits));
    if(row<0.0) {
        rgba[0]=(uint8_t)((bits>>16)&0xffu);
        rgba[1]=(uint8_t)((bits>>8)&0xffu);
        rgba[2]=(uint8_t)(bits&0xffu);
    } else {
        /* The top byte's quadrant also carries fixed green and blue so that a
         * value whose high byte is zero is still ink on this black clear colour
         * instead of an image indistinguishable from no draw. */
        rgba[0]=(uint8_t)((bits>>24)&0xffu);
        rgba[1]=128u;
        rgba[2]=64u;
    }
    rgba[3]=255u;
}

unsigned ps5vk_geometry_witness_read_pixel(unsigned item,unsigned extent)
{
    if(!extent)return 0u;
    const double cx=read_bytes_column(read_bytes_vertex_x(item));
    double pixel=(cx+1.0)*0.5*(double)extent;
    if(pixel<0.0)pixel=0.0;
    if(pixel>(double)(extent-1u))pixel=(double)(extent-1u);
    return (unsigned)pixel;
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
    /* The same vertex the place uses: the marker's colour is the value it read,
     * so pairing marker 0's place (+1.2) with the other vertex's colour (-1.2)
     * would demand the wrong image and refuse a correct one. */
    const double px=marker?-1.2:1.2;
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

/* The point family's marker for one input point: the axis-aligned quad
 * runtime_geometry_points.geom emits around the position it read. */
static int family_point_covers(unsigned point,double ndc_x,double ndc_y)
{
    if(point>=FAMILY_POINTS)return 0;
    double position[2],colour[3];
    ps5vk_geometry_witness_family_vertex(point,position,colour);
    return ndc_x>=position[0]-family_point_extent &&
           ndc_x<=position[0]+family_point_extent &&
           ndc_y>=position[1]-family_point_extent &&
           ndc_y<=position[1]+family_point_extent;
}

/* The line family's marker for one input segment, mirrored from
 * runtime_geometry_lines.geom: centred on the segment, half extent equal to
 * half the segment plus a fixed margin, so a stage that read only its first
 * vertex produces a marker of the wrong size at the wrong place. */
static void family_line_marker(unsigned segment,double centre[2],double extent[2],
    double colour[3])
{
    double a[2],b[2],ac[3],bc[3];
    ps5vk_geometry_witness_family_vertex(2u*segment,a,ac);
    ps5vk_geometry_witness_family_vertex(2u*segment+1u,b,bc);
    centre[0]=(a[0]+b[0])*0.5;
    centre[1]=(a[1]+b[1])*0.5;
    /* abs(b-a)*0.5 + 0.1, the shader's expression. */
    extent[0]=((b[0]>a[0]?b[0]-a[0]:a[0]-b[0])*0.5)+0.1;
    extent[1]=((b[1]>a[1]?b[1]-a[1]:a[1]-b[1])*0.5)+0.1;
    for(unsigned i=0;i<3;++i)colour[i]=(ac[i]+bc[i])*0.5;
}

static int family_line_covers(unsigned segment,double ndc_x,double ndc_y)
{
    if(segment>=FAMILY_SEGMENTS)return 0;
    double centre[2],extent[2],colour[3];
    family_line_marker(segment,centre,extent,colour);
    return ndc_x>=centre[0]-extent[0] && ndc_x<=centre[0]+extent[0] &&
           ndc_y>=centre[1]-extent[1] && ndc_y<=centre[1]+extent[1];
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
    /* The readback writes two quadrants per input primitive at the place the
     * value's own sign bit puts them, so a correct run covers exactly the
     * squares the oracle knows and a read that returned anything else changes
     * the bytes there rather than moving the ink out of sight. */
    case PS5VK_GEOMETRY_READ_V0:
    case PS5VK_GEOMETRY_READ_V1:
    case PS5VK_GEOMETRY_READ_V2: {
        const unsigned index=(unsigned)read_bytes_index(witness_case);
        for(unsigned p=0;p<READ_PRIMITIVES;++p)for(unsigned r=0;r<2u;++r)
            if(read_bytes_covers(read_bytes_expected(index,p),read_bytes_rows[r],
                                 ndc_x,ndc_y))
                return 1;
        return 0;
    }
    /* The envelope's ribbon tiles this band: x within the columns the 256
     * vertices span, y between the two edges they alternate between. */
    case PS5VK_GEOMETRY_ENVELOPE:
        return ndc_x>=-0.75 && ndc_x<=0.75 && ndc_y>=-0.25 && ndc_y<=0.25;
    /* The invocations case covers one column per invocation, each as wide as the
     * marker that invocation emits. */
    case PS5VK_GEOMETRY_INVOCATIONS:
        for(unsigned k=0;k<INVOCATION_COUNT;++k) {
            const double column=-0.72+0.045*(double)k;
            if(ndc_x>=column-0.02 && ndc_x<=column+0.02 &&
               ndc_y>=-0.2 && ndc_y<=0.2)return 1;
        }
        return 0;
    /* The components case draws the same centred quad the constant case does,
     * with the colour its own inputs produce. */
    case PS5VK_GEOMETRY_COMPONENTS:
        return ndc_x>=-0.5 && ndc_x<=0.5 && ndc_y>=-0.5 && ndc_y<=0.5;
    /* The per-primitive id: one column per primitive of the witness draw, placed
     * by the id the stage read. */
    case PS5VK_GEOMETRY_PRIMITIVE_ID:
        for(unsigned id=0;id<PRIMITIVE_COUNT;++id) {
            const double column=-0.6+0.6*(double)id;
            if(ndc_x>=column-0.02 && ndc_x<=column+0.02 &&
               ndc_y>=-0.2 && ndc_y<=0.2)return 1;
        }
        return 0;
    /* The input families: real markers at the places the items the stage read
     * put them, so a read of the wrong item moves the ink instead of leaving
     * the target empty. */
    case PS5VK_GEOMETRY_POINTS:
        for(unsigned point=0;point<FAMILY_POINTS;++point)
            if(family_point_covers(point,ndc_x,ndc_y))return 1;
        return 0;
    case PS5VK_GEOMETRY_LINES:
        for(unsigned segment=0;segment<FAMILY_SEGMENTS;++segment)
            if(family_line_covers(segment,ndc_x,ndc_y))return 1;
        return 0;
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
    if(read_bytes_index(witness_case)>=0) {
        /* The pixel is inside one of the four quadrants a correct run covers;
         * its colour is the raw bytes of the value the stage read there. A read
         * that returned another value puts the quadrant somewhere else, which
         * the coverage half of the verdict reports as missing or foreign
         * pixels, and its colour is then reported verbatim in the log. */
        const unsigned index=(unsigned)read_bytes_index(witness_case);
        for(unsigned p=0;p<READ_PRIMITIVES;++p)for(unsigned r=0;r<2u;++r) {
            if(!read_bytes_covers(read_bytes_expected(index,p),read_bytes_rows[r],
                                  2.0*u-1.0,2.0*v-1.0))
                continue;
            read_bytes_colour(read_bytes_expected(index,p),read_bytes_rows[r],rgba);
            return;
        }
        rgba[0]=rgba[1]=rgba[2]=0u;
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_ENVELOPE) {
        /* The envelope stage emits a constant colour, so the band is uniform:
         * the verdict is "the whole band the 256 vertices span is covered with
         * that colour", which a stage that emitted fewer vertices cannot make. */
        rgba[0]=unorm8(0.25);
        rgba[1]=unorm8(0.5);
        rgba[2]=unorm8(0.75);
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_INVOCATIONS) {
        /* Each column carries the colour of the invocation id that placed it, so
         * a wrong id, a missing invocation and a stage that ran once are three
         * different images rather than one missing marker. */
        for(unsigned k=0;k<INVOCATION_COUNT;++k) {
            const double column=-0.72+0.045*(double)k;
            if(2.0*u-1.0<column-0.02 || 2.0*u-1.0>column+0.02)continue;
            if(2.0*v-1.0<-0.2 || 2.0*v-1.0>0.2)continue;
            rgba[0]=unorm8((double)k/31.0);
            rgba[1]=unorm8(0.5);
            rgba[2]=unorm8(0.25);
            rgba[3]=255u;
            return;
        }
        rgba[0]=rgba[1]=rgba[2]=0u;
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_PRIMITIVE_ID) {
        /* The marker carries the id it read, so a constant or stale id is a
         * different image rather than a missing one. */
        for(unsigned id=0;id<PRIMITIVE_COUNT;++id) {
            const double column=-0.6+0.6*(double)id;
            if(2.0*u-1.0<column-0.02 || 2.0*u-1.0>column+0.02)continue;
            if(2.0*v-1.0<-0.2 || 2.0*v-1.0>0.2)continue;
            rgba[0]=unorm8((double)id);
            rgba[1]=unorm8(0.5);
            rgba[2]=unorm8(0.25);
            rgba[3]=255u;
            return;
        }
        rgba[0]=rgba[1]=rgba[2]=0u;
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_COMPONENTS) {
        /* Both halves of the component envelope are in this image. The geometry
         * stage folds the sixty-four components it READ (the pre-raster stage's
         * sixteen vec4s, whose values sum to 576) into the varying at location
         * zero, so blue is 576/4096. It then WRITES sixty-four components of its
         * own at locations 1..16, every one of them 1.0, and the pixel half
         * reads all sixteen locations and folds them: red is the plain sum
         * (64/128) and green the location-weighted one (136/272). Red therefore
         * moves by eight unorm steps if one location is dropped, and green moves
         * if two locations are exchanged, which is what makes the OUTPUT side of
         * the envelope observable instead of merely declared. */
        rgba[0]=unorm8(64.0/128.0);
        rgba[1]=unorm8(136.0/272.0);
        rgba[2]=unorm8(576.0/4096.0);
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_POINTS) {
        /* The marker carries the colour of the point it was built from, so a
         * read that returned another item shows up as that item's colour at
         * this place, not as a missing pixel. */
        for(unsigned point=0;point<FAMILY_POINTS;++point) {
            double position[2],colour[3];
            if(!family_point_covers(point,2.0*u-1.0,2.0*v-1.0))continue;
            ps5vk_geometry_witness_family_vertex(point,position,colour);
            rgba[0]=unorm8(colour[0]);
            rgba[1]=unorm8(colour[1]);
            rgba[2]=unorm8(colour[2]);
            rgba[3]=255u;
            return;
        }
        rgba[0]=rgba[1]=rgba[2]=0u;
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_LINES) {
        for(unsigned segment=0;segment<FAMILY_SEGMENTS;++segment) {
            double centre[2],extent[2],colour[3];
            if(!family_line_covers(segment,2.0*u-1.0,2.0*v-1.0))continue;
            family_line_marker(segment,centre,extent,colour);
            rgba[0]=unorm8(colour[0]);
            rgba[1]=unorm8(colour[1]);
            rgba[2]=unorm8(colour[2]);
            rgba[3]=255u;
            return;
        }
        rgba[0]=rgba[1]=rgba[2]=0u;
        rgba[3]=255u;
        return;
    }
    if(witness_case==PS5VK_GEOMETRY_SHRINK) {
        /* The stage scales the POSITIONS and keeps the varying, so the varying
         * interpolates over the scaled triangle, not over the pixel's own place
         * in the target: the vertex at NDC p carries ((p+1)/2), which over the
         * scaled triangle is (screen + 0.5), i.e. 2u-0.5 and 2v-0.5 here. The
         * hardware interpolates exactly that (measured: the shrunk image's
         * colours are 0 where this function is negative), so the oracle said the
         * wrong thing about a correct image until now. */
        rgba[0]=unorm8(2.0*u-0.5);
        rgba[1]=unorm8(2.0*v-0.5);
        rgba[2]=unorm8(0.5);
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
    /* Same shape for the readback: four small squares, so the verdict is real
     * coverage plus the exact bytes the stage read at each of them. */
    case PS5VK_GEOMETRY_READ_V0:
    case PS5VK_GEOMETRY_READ_V1:
    case PS5VK_GEOMETRY_READ_V2:
    /* The envelope's band is real coverage of a known shape, neither empty nor
     * the whole target. */
    case PS5VK_GEOMETRY_ENVELOPE:
    /* The invocations case covers 32 real columns of a known shape, neither
     * empty nor the whole target. */
    case PS5VK_GEOMETRY_INVOCATIONS:
    /* The components case draws a known quad, neither empty nor the whole
     * target. */
    case PS5VK_GEOMETRY_COMPONENTS:
    /* The per-primitive id case draws two known columns, neither empty nor the
     * whole target. */
    case PS5VK_GEOMETRY_PRIMITIVE_ID:
    /* The point family draws four markers and the line family three, each a
     * known shape at a place the item it read puts it: real coverage, neither
     * empty (the stage did not run, or read nothing) nor the whole target. */
    case PS5VK_GEOMETRY_POINTS:
    case PS5VK_GEOMETRY_LINES:
        return witness->expected_covered>0u && witness->expected_covered<pixels;
    case PS5VK_GEOMETRY_SUPPRESS:
        return witness->expected_covered==0u && witness->covered==0u;
    }
    return 0;
}

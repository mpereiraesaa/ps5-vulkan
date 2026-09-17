#version 450
/* Diagnostic-only pixel half for the geometry component envelope.
 *
 * The output side of the component minimum is only observable if something
 * READS what the geometry stage wrote: a stage that declares sixty-four output
 * components and a pixel stage that reads three of them cannot tell "the other
 * sixty-one were delivered" from "they were dropped", because the image is the
 * same either way. This stage therefore declares an input for every one of the
 * sixteen vec4 outputs the geometry half writes and folds all sixty-four
 * components into the colour:
 *
 *   red   = total / 128     - the plain sum of all 64 components (64 when every
 *                             one arrives as 1.0), so a location that was
 *                             dropped changes red by 4/128 = 8 unorm steps;
 *   green = weighted / 272  - the location-weighted sum, sum(k * o_k.x), which
 *                             is 136 when every location carries its own value,
 *                             so an exchange between two locations keeps the
 *                             total but changes green;
 *   blue  = the three-component varying the geometry half built from its own
 *           sixty-four INPUT components, which keeps the input side of the
 *           envelope in the same image.
 *
 * Both sums are exactly representable for the values the geometry stage writes,
 * so the oracle predicts the whole image from the declaration alone.
 */
layout(location = 0) in vec3 in_v;
layout(location = 1) in vec4 i1;
layout(location = 2) in vec4 i2;
layout(location = 3) in vec4 i3;
layout(location = 4) in vec4 i4;
layout(location = 5) in vec4 i5;
layout(location = 6) in vec4 i6;
layout(location = 7) in vec4 i7;
layout(location = 8) in vec4 i8;
layout(location = 9) in vec4 i9;
layout(location = 10) in vec4 i10;
layout(location = 11) in vec4 i11;
layout(location = 12) in vec4 i12;
layout(location = 13) in vec4 i13;
layout(location = 14) in vec4 i14;
layout(location = 15) in vec4 i15;
layout(location = 16) in vec4 i16;
layout(location = 0) out vec4 out_color;
void main()
{
    vec4 s1 = i1 + i2 + i3 + i4;
    vec4 s2 = i5 + i6 + i7 + i8;
    vec4 s3 = i9 + i10 + i11 + i12;
    vec4 s4 = i13 + i14 + i15 + i16;
    float total = (s1 + s2 + s3 + s4).x + (s1 + s2 + s3 + s4).y +
                  (s1 + s2 + s3 + s4).z + (s1 + s2 + s3 + s4).w;
    float weighted = i1.x + 2.0 * i2.x + 3.0 * i3.x + 4.0 * i4.x +
                     5.0 * i5.x + 6.0 * i6.x + 7.0 * i7.x + 8.0 * i8.x +
                     9.0 * i9.x + 10.0 * i10.x + 11.0 * i11.x + 12.0 * i12.x +
                     13.0 * i13.x + 14.0 * i14.x + 15.0 * i15.x + 16.0 * i16.x;
    out_color = vec4(total / 128.0, weighted / 272.0, in_v.x, 1.0);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
// DXVK262-T10 render witness: three quads built from gl_VertexIndex, six
// vertices each, like DXVK's vertex-buffer-less first draw. Quad 0 covers
// NDC x [-1, 0.5] over the whole height; quads 1 and 2 cover x [0.5, 1] over
// NDC y [-1, 0] and [0, 1]. Quad 2's triangles are wound the other way, so the
// back-face cull removes exactly one of the two marker quads.
layout(location = 0) flat out uint quad;
layout(set=0,binding=0,std140) uniform B0 { uvec4 words[16]; } b0;
layout(set=0,binding=1,std140) uniform B1 { uvec4 words[16]; } b1;
layout(set=0,binding=2,std140) uniform B2 { uvec4 words[16]; } b2;
layout(set=0,binding=3,std140) uniform B3 { uvec4 words[16]; } b3;
uvec4 expected(uint base) { return uvec4(0x4b000001u+base*37u)+uvec4(0u,37u,74u,111u); }
void main()
{
    uint q = uint(gl_VertexIndex) / 6u;
    uint k = uint(gl_VertexIndex) % 6u;
    if (q == 2u) k = (k / 3u) * 3u + 2u - k % 3u;
    float cx = (k == 1u || k == 4u || k == 5u) ? 1.0 : 0.0;
    float cy = (k == 2u || k == 3u || k == 5u) ? 1.0 : 0.0;
    vec2 lo = q == 0u ? vec2(-1.0, -1.0) : (q == 1u ? vec2(0.5, -1.0) : vec2(0.5, 0.0));
    vec2 hi = q == 0u ? vec2(0.5, 1.0) : (q == 1u ? vec2(1.0, 0.0) : vec2(1.0, 1.0));
    gl_Position = vec4(mix(lo, hi, vec2(cx, cy)), 0.5, 1.0);
    uvec4 differences=uvec4(0u);
    for(uint i=0u;i<16u;++i) {
        differences |= b0.words[i]^expected(i*4u);
        differences |= b1.words[i]^expected(64u+i*4u);
        differences |= b2.words[i]^expected(128u+i*4u);
        differences |= b3.words[i]^expected(192u+i*4u);
    }
    if(any(notEqual(differences,uvec4(0u)))) gl_Position=vec4(2.0,2.0,0.5,1.0);
    quad = q;
}

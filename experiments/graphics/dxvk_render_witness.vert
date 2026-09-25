#version 450
// DXVK262-T10 render witness: three quads built from gl_VertexIndex, six
// vertices each, like DXVK's vertex-buffer-less first draw. Quad 0 covers
// NDC x [-1, 0.5] over the whole height; quads 1 and 2 cover x [0.5, 1] over
// NDC y [-1, 0] and [0, 1]. Quad 2's triangles are wound the other way, so the
// back-face cull removes exactly one of the two marker quads.
layout(location = 0) flat out uint quad;
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
    quad = q;
}

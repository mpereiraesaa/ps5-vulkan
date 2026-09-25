#version 450
/* T09 depth/stencil witness: one full-target quad (64x64) whose depth is the
 * plane z = (px + 64 * py) / 8192 in pixel coordinates, so every pixel centre
 * gets its own depth value and a misplaced texel cannot pass the readback. */
void main()
{
    const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
                                    vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 ndc = corners[gl_VertexIndex];
    vec2 pixel = (ndc + 1.0) * 32.0;
    gl_Position = vec4(ndc, (pixel.x + 64.0 * pixel.y) / 8192.0, 1.0);
}

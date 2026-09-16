#version 450
#extension GL_EXT_multiview : require

/* Private six-view witness vertex stage (T02-D1b). It is the only stage that
 * reads gl_ViewIndex, and everything it writes is a function of that index, so
 * a layer's colour and depth are evidence about WHICH view rendered into it:
 * red is (view + 1) / 16 and green is view / 8, both of which land on distinct
 * UNORM8 bytes for the six views this profile is allowed to name, and the depth
 * is the same (view + 1) / 16, which is exact in D32 and distinct per view. All
 * three vertices carry the same value and the triangle is fullscreen, so a
 * layer's colour is that layer's value everywhere it is covered - which is what
 * lets the readback prove identity, order, coverage and the absence of aliasing
 * between layers instead of summing them. */
layout(location = 0) out vec3 color;

void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    const float shade = float(gl_ViewIndex);
    gl_Position = vec4(positions[gl_VertexIndex], (shade + 1.0) / 16.0, 1.0);
    color = vec3((shade + 1.0) / 16.0, shade / 8.0, 0.5);
}

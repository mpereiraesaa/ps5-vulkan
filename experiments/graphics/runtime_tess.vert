#version 450
/* Tessellation witness, pre-raster half. One patch control point per
 * gl_VertexIndex, with a varying affine in the position so the tessellated
 * image of a patch is predictable from the same geometry the other witnesses
 * use. */
layout(location = 0) out vec3 color;
void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(0.0, 1.0));
    const vec3 colors[3] = vec3[3](
        vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));
    int i = gl_VertexIndex % 3;
    vec2 p = positions[i];
    gl_Position = vec4(p, 0.5, 1.0);
    color = colors[i];
}

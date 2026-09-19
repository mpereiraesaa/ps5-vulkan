#version 450
/* The geometry witness's pre-raster half: two triangles cover the whole target
 * with a varying affine in the position, so the oracle can predict every pixel
 * the geometry stage keeps, moves or rewrites. */
layout(location = 0) out vec3 color;
void main()
{
    const vec2 positions[6] = vec2[6](
        vec2(-1.2, -1.2), vec2(1.2, -1.2), vec2(-1.2, 1.2),
        vec2(1.2, -1.2), vec2(1.2, 1.2), vec2(-1.2, 1.2));
    vec2 p = positions[gl_VertexIndex % 6];
    gl_Position = vec4(p, 0.5, 1.0);
    color = vec3((p.x + 1.0) * 0.5, (p.y + 1.0) * 0.5, 0.5);
}

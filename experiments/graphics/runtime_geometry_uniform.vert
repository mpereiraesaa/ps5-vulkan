#version 450
/* Diagnostic-only pre-raster half for the geometry descriptor case: the two
 * triangles the witness family uses, plus the varying the geometry half
 * multiplies by the uniform buffer it reads. */
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

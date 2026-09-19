#version 450
layout(location = 0) out vec3 color;
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[1]; };
void main()
{
    const vec3 positions[3] = vec3[3](vec3(-0.7, -0.6, 0.5), vec3(0.7, -0.6, 0.5), vec3(0.0, 0.7, 0.5));
    const vec3 colors[3] = vec3[3](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));
    int i = gl_VertexIndex % 3;
    gl_Position = vec4(positions[i], 1.0);
    color = colors[i];
    /* The triangle is wholly inside or wholly outside the plane, so culling
     * discards the primitive instead of clipping it: the two outcomes differ
     * by the interior pixels the primitive would otherwise have covered. */
    gl_CullDistance[0] = positions[i].x + 0.25;
}

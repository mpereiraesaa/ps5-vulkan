#version 450
layout(location = 0) out vec3 color;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[2]; };
void main()
{
    const vec3 positions[3] = vec3[3](vec3(-0.7, -0.6, 0.5), vec3(0.7, -0.6, 0.5), vec3(0.0, 0.7, 0.5));
    const vec3 colors[3] = vec3[3](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));
    int i = gl_VertexIndex % 3;
    gl_Position = vec4(positions[i], 1.0);
    color = colors[i];
    /* A plane and its mirror: exactly one distance is negative on either side
     * of x = 0, so a clipped fragment and an interpolated one are
     * distinguishable from each other by a single witnessed pixel. */
    gl_ClipDistance[0] = positions[i].x + 0.25;
    gl_ClipDistance[1] = 0.25 - positions[i].x;
}

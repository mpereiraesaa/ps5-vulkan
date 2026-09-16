#version 450
layout(location = 0) out vec3 color;
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[4];
    float gl_CullDistance[4];
};
void main()
{
    const vec3 positions[3] = vec3[3](vec3(-0.7, -0.6, 0.5), vec3(0.7, -0.6, 0.5), vec3(0.0, 0.7, 0.5));
    const vec3 colors[3] = vec3[3](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));
    int i = gl_VertexIndex % 3;
    gl_Position = vec4(positions[i], 1.0);
    color = colors[i];
    /* The declared widths are the profile ceiling: four clip plus four cull
     * components, the two packed position registers the pre-raster stage can
     * export past POS0. Only the sign of each distance matters here. */
    for (int plane = 0; plane < 4; ++plane) {
        gl_ClipDistance[plane] = positions[i].x + 1.0 + float(plane);
        gl_CullDistance[plane] = positions[i].x + 1.0 + float(plane);
    }
}

#version 450

layout(location = 0) out vec2 pixel_position;

void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexIndex];
    gl_Position = vec4(position, 0.5, 1.0);
    /* Interpolation maps NDC [-1,1] to pixel coordinates [0,64].  Pixel
     * centres therefore arrive as n+0.5 and uint conversion recovers n. */
    pixel_position = (position + 1.0) * 32.0;
}

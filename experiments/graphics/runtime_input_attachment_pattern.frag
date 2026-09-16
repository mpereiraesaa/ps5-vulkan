#version 450

layout(location = 0) in vec2 pixel_position;
layout(location = 0) out vec4 output_color;

void main()
{
    /* The bounded graphics ABI intentionally exposes no fragment built-ins.
     * Use the vertex-to-fragment interface the runtime already validates. */
    uint x = uint(pixel_position.x);
    uint y = uint(pixel_position.y);
    uint red = (x * 17u + y * 3u) & 255u;
    uint green = (y * 29u + 7u) & 255u;
    uint blue = ((x * 5u) ^ (y * 11u)) & 255u;
    /* R8G8B8A8 memory is the little-endian oracle word's B,G,R,A bytes. */
    output_color = vec4(blue, green, red, 255u) / 255.0;
}

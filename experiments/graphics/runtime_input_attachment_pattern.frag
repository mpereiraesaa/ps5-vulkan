#version 450

layout(location = 0) out vec4 output_color;

void main()
{
    uint x = uint(gl_FragCoord.x);
    uint y = uint(gl_FragCoord.y);
    uint red = (x * 17u + y * 3u) & 255u;
    uint green = (y * 29u + 7u) & 255u;
    uint blue = ((x * 5u) ^ (y * 11u)) & 255u;
    /* R8G8B8A8 memory is the little-endian oracle word's B,G,R,A bytes. */
    output_color = vec4(blue, green, red, 255u) / 255.0;
}

#version 450

layout(set = 0, binding = 0, std430) buffer OutputBuffer {
    int value[1024];
} output_buffer;

layout(location = 0) out vec4 out_color;

void main()
{
    const ivec2 coord = ivec2(gl_FragCoord);
    const int index = coord.y * 32 + coord.x;
    output_buffer.value[index] = 1;
    discard;
    out_color = vec4(0.0, 0.0, 1.0, 1.0);
}

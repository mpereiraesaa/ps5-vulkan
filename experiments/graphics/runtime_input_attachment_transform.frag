#version 450

layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput source_color;
layout(location = 0) out vec4 output_color;

void main()
{
    vec4 source = subpassLoad(source_color);
    /* Oracle word transform: R'=255-G, G'=B, B'=255-R.  Components are
     * written in the R8 memory order B',G',R',A. */
    output_color = vec4(1.0 - source.b, source.r, 1.0 - source.g, 1.0);
}

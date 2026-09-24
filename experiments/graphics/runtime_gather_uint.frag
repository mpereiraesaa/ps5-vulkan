#version 450
layout(set=0,binding=0) uniform usampler2D source_texture;
layout(location=0) out uvec4 output_color;
void main()
{
    output_color=textureGather(source_texture,vec2(0.5));
}

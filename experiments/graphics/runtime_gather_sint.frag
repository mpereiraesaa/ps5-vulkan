#version 450
layout(set=0,binding=0) uniform isampler2D source_texture;
layout(location=0) out ivec4 output_color;
void main()
{
    output_color=textureGather(source_texture,vec2(0.5));
}

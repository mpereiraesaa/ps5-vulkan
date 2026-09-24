#version 450
#extension GL_ARB_texture_gather : require
layout(set=0,binding=0) uniform sampler2D source_texture;
layout(location=0) out vec4 output_color;
void main()
{
    output_color=textureGather(source_texture,vec2(0.5),2);
}

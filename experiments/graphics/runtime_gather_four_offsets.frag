#version 450
#extension GL_ARB_texture_gather : require
layout(set=0,binding=0) uniform sampler2D source_texture;
layout(location=0) out vec4 output_color;
void main()
{
    const ivec2 offsets[4]=ivec2[4](ivec2(-8,-8),ivec2(7,-8),ivec2(-8,7),ivec2(7,7));
    output_color=textureGatherOffsets(source_texture,vec2(0.5),offsets,2);
}

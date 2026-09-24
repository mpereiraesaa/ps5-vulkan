#version 450
layout(set=0,binding=0) uniform sampler2D source_texture;
layout(location=0) out vec4 output_color;
void main()
{
    // Core form: implicit component zero.  Explicit component selection is
    // covered separately because SPIR-V requires ImageGatherExtended for it.
    output_color=textureGather(source_texture,vec2(0.5));
}

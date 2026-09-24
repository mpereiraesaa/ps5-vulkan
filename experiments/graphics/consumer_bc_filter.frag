#version 450
layout(set=0, binding=0) uniform sampler2D bc_texture;
layout(location=0) out vec4 color;
void main()
{
    color = textureLod(bc_texture, gl_FragCoord.xy / vec2(64.0), 0.0);
}

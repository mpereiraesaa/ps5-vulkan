#version 450
layout(set = 0, binding = 0) uniform sampler2D source_texture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 output_color;
void main()
{
    float lod = uv.x < 0.333333 ? 0.0 : (uv.x < 0.666667 ? 1.0 : 2.0);
    output_color = textureLod(source_texture, uv, lod);
}

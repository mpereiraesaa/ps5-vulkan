#version 450

layout(set = 0, binding = 0) uniform samplerCubeArray cube_texture;
layout(location = 0) out vec4 color;

void main()
{
    color = texture(cube_texture, vec4(1.0, 0.0, 0.0, 0.0));
}

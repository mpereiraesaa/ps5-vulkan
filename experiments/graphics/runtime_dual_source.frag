#version 450

layout(location = 0, index = 0) out vec4 primary_color;
layout(location = 0, index = 1) out vec4 secondary_color;

void main()
{
    primary_color = vec4(0.25, 0.50, 0.75, 1.0);
    secondary_color = vec4(0.80, 0.40, 0.20, 0.50);
}

#version 450

layout(location = 0) out vec4 mrt0_color;
layout(location = 1) out vec4 mrt1_color;

void main()
{
    mrt0_color = vec4(0.25, 0.50, 0.75, 1.0);
    mrt1_color = vec4(0.80, 0.40, 0.20, 0.50);
}

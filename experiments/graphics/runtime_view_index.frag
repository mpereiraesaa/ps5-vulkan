#version 450
#extension GL_EXT_multiview : require
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(color.r, float(gl_ViewIndex) / 8.0, color.b, 1.0);
}

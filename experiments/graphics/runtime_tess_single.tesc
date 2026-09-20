#version 450
layout(vertices = 1) out;
void main()
{
    gl_TessLevelInner[0] = 5.0;
    gl_TessLevelInner[1] = 5.0;
    gl_TessLevelOuter[0] = 5.0;
    gl_TessLevelOuter[1] = 5.0;
    gl_TessLevelOuter[2] = 5.0;
    gl_TessLevelOuter[3] = 5.0;
}

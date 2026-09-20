#version 450
layout(triangles, equal_spacing, ccw) in;
void main()
{
    gl_Position = vec4(gl_TessCoord.xy * 2.0 - 1.0, 0.0, 1.0);
}

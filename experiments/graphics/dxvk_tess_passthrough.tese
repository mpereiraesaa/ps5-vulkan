// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(triangles,equal_spacing,ccw) in;
layout(location=0) in uint input_quad[];
layout(location=0) flat out uint output_quad;
void main()
{
    gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+
                gl_TessCoord.y*gl_in[1].gl_Position+
                gl_TessCoord.z*gl_in[2].gl_Position;
    output_quad=input_quad[0];
}

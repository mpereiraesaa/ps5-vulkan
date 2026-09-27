// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(vertices=3) out;
layout(location=0) in uint input_quad[];
layout(location=0) out uint output_quad[];
void main()
{
    gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;
    output_quad[gl_InvocationID]=input_quad[gl_InvocationID];
    if(gl_InvocationID==0) {
        gl_TessLevelOuter[0]=1.0;
        gl_TessLevelOuter[1]=1.0;
        gl_TessLevelOuter[2]=1.0;
        gl_TessLevelInner[0]=1.0;
    }
}

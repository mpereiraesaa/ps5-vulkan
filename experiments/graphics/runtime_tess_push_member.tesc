// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(vertices=3) out;
layout(location=0) in vec3 color[];
layout(location=0) out vec3 delivered[];
layout(push_constant) uniform Push {
    layout(offset=16) vec4 hull[4];
    layout(offset=80) int selected;
} pc;
void main() {
    int index=pc.selected;
    gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;
    // Push arrays require dynamically uniform indexing. Runtime selected=1
    // chooses the second value; an ignored index or prefix changes the oracle.
    delivered[gl_InvocationID]=color[gl_InvocationID]+
        pc.hull[index].xyz-vec3(0.125);
    if(gl_InvocationID==0) {
        gl_TessLevelOuter[0]=2;gl_TessLevelOuter[1]=2;
        gl_TessLevelOuter[2]=2;gl_TessLevelInner[0]=1;
    }
}

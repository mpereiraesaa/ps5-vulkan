// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(location=0) out vec3 color;
void main() {
    // Wide indices65541/65538/65543 with baseVertex=-65536 ->5/2/7.
    // Two instances start at3; neither zero-based IDs nor ignored indices
    // can generate the two independently expected triangles.
    vec2 uv=vec2(4.0);
    if(gl_VertexIndex==5)uv=vec2(0,0);
    if(gl_VertexIndex==2)uv=vec2(1,0);
    if(gl_VertexIndex==7)uv=vec2(0,1);
    int patch_id=gl_InstanceIndex-3;
    gl_Position=vec4(uv.x*0.8-0.9+float(patch_id),uv.y*1.8-0.9,0,1);
    color=vec3(uv,patch_id==0?0.25:0.75);
}

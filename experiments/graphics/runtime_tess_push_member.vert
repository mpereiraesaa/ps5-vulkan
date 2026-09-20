// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(location=0) out vec3 color;
layout(push_constant) uniform Push { vec4 vertex; } pc;
void main() {
    vec2 uv[3]=vec2[3](vec2(0,0),vec2(1,0),vec2(0,1));
    gl_Position=vec4(uv[gl_VertexIndex]*1.8-0.9,0,1);
    color=vec3(uv[gl_VertexIndex],0.5)+pc.vertex.xyz-vec3(0.125,0.25,0.5);
}

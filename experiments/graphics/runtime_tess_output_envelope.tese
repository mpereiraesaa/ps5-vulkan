// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(triangles,equal_spacing,cw) in;
out gl_PerVertex { vec4 gl_Position; };
layout(location=0) out vec4 values[31];
void main() {
    vec2 p=gl_TessCoord.xy;
    gl_Position=vec4(p*1.8-0.9,0,1);
    for(int i=0;i<31;i++)
        values[i]=vec4(p.x,p.y,p.x+p.y,0.5)+float(i);
}

// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(location=0) in vec4 values[31];
layout(location=0) out vec4 color;
void main() {
    vec2 p=values[0].xy;
    bool valid=true;
    for(int i=0;i<31;i++) {
        vec4 expected=vec4(p.x,p.y,p.x+p.y,0.5)+float(i);
        if(any(greaterThan(abs(values[i]-expected),vec4(0.002)))) valid=false;
    }
    color=valid?vec4(p,0.5,1):vec4(1,0,1,1);
}

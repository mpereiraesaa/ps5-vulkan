// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(vertices=3) out;
void main() {
    if(gl_InvocationID==0) {
        float level=MATRIX_SPACING==2?3.0:2.0;
        for(int i=0;i<4;i++)gl_TessLevelOuter[i]=level;
        for(int i=0;i<2;i++)gl_TessLevelInner[i]=level;
    }
}

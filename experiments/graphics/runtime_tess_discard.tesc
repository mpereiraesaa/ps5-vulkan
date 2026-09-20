// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(vertices=3) out;
void main() {
    if(gl_InvocationID==0) {
        float outer[4]=float[4](3,3,3,3);
        float inner[2]=float[2](3,3);
        int patch_id=gl_PrimitiveID;
        if(patch_id>=1 && patch_id<=12) {
            int field=(patch_id-1)/3,kind=(patch_id-1)%3;
            outer[field]=kind==0?0.0:kind==1?-0.5:uintBitsToFloat(0x7fc00000u);
        } else if(patch_id>=13 && patch_id<=16) {
            inner[(patch_id-13)/2]=(patch_id&1)==1?0.0:-0.5;
        }
        for(int i=0;i<4;i++)gl_TessLevelOuter[i]=outer[i];
        for(int i=0;i<2;i++)gl_TessLevelInner[i]=inner[i];
    }
}

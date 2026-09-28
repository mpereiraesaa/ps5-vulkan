// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
// The ordinary witness image, with coefficients and guards supplied inline.
layout(location=0) flat in uint quad;
layout(location=0) out vec4 color;
layout(set=0,binding=0,std140) uniform B0 { uvec4 words[16]; } b0;
layout(set=0,binding=1,std140) uniform B1 { uvec4 words[16]; } b1;
layout(set=0,binding=2,std140) uniform B2 { uvec4 words[16]; } b2;
layout(set=0,binding=3,std140) uniform B3 { uvec4 words[16]; } b3;
void main()
{
    uint index=(uint(gl_FragCoord.x)%16u)+(uint(gl_FragCoord.y)%16u)*16u;
    uint j=index%64u;
    uint actual;
    if(index<64u) actual=b0.words[j/4u][j%4u];
    else if(index<128u) actual=b1.words[j/4u][j%4u];
    else if(index<192u) actual=b2.words[j/4u][j%4u];
    else actual=b3.words[j/4u][j%4u];
    if(actual!=0x4b000001u+index*37u) {
        color=vec4(1.0,0.0,1.0,1.0);
    } else if(quad==0u) {
        color=vec4(floor(gl_FragCoord.x)*(float(4u)/255.0),
                   floor(gl_FragCoord.y)*(float(4u)/255.0),
                   float(64u)/255.0,float(255u)/255.0);
    } else if(quad==1u) {
        color=vec4(0.0,float(255u)/255.0,0.0,float(255u)/255.0);
    } else {
        color=vec4(0.0,0.0,float(255u)/255.0,float(255u)/255.0);
    }
}

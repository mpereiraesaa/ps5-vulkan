// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
// DXVK first-draw geometry with a deliberately wider producer interface.
layout(location=0) flat out uvec4 quad_payload;
void main()
{
    uint q=uint(gl_VertexIndex)/6u;
    uint k=uint(gl_VertexIndex)%6u;
    if(q==2u) k=(k/3u)*3u+2u-k%3u;
    float cx=(k==1u||k==4u||k==5u)?1.0:0.0;
    float cy=(k==2u||k==3u||k==5u)?1.0:0.0;
    vec2 lo=q==0u?vec2(-1.0,-1.0):(q==1u?vec2(0.5,-1.0):vec2(0.5,0.0));
    vec2 hi=q==0u?vec2(0.5,1.0):(q==1u?vec2(1.0,0.0):vec2(1.0,1.0));
    gl_Position=vec4(mix(lo,hi,vec2(cx,cy)),0.5,1.0);
    quad_payload=uvec4(q,q^0x55u,0xdeadbeefu,0xaabbccddu);
}

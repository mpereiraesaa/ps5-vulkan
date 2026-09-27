// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(triangles,equal_spacing,ccw) in;
layout(location=0) in uint input_quad[];
layout(location=0) flat out uint output_quad;
layout(set=0,binding=0,std140) uniform B0 { uvec4 words[16]; } b0;
layout(set=0,binding=1,std140) uniform B1 { uvec4 words[16]; } b1;
layout(set=0,binding=2,std140) uniform B2 { uvec4 words[16]; } b2;
layout(set=0,binding=3,std140) uniform B3 { uvec4 words[16]; } b3;
uvec4 expected(uint base) { return uvec4(0x4b000001u+base*37u)+uvec4(0u,37u,74u,111u); }
void main()
{
    gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+
                gl_TessCoord.y*gl_in[1].gl_Position+
                gl_TessCoord.z*gl_in[2].gl_Position;
    uvec4 differences=uvec4(0u);
    for(uint i=0u;i<16u;++i) {
        differences |= b0.words[i]^expected(i*4u);
        differences |= b1.words[i]^expected(64u+i*4u);
        differences |= b2.words[i]^expected(128u+i*4u);
        differences |= b3.words[i]^expected(192u+i*4u);
    }
    if(any(notEqual(differences,uvec4(0u)))) gl_Position=vec4(2.0,2.0,0.5,1.0);
    output_quad=input_quad[0];
}

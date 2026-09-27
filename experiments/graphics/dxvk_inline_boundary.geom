// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices=3) out;
layout(location=0) flat in uint input_quad[];
layout(location=0) flat out uint output_quad;
layout(set=0,binding=0,std140) uniform B0 { uvec4 words[16]; } b0;
layout(set=0,binding=1,std140) uniform B1 { uvec4 words[16]; } b1;
layout(set=0,binding=2,std140) uniform B2 { uvec4 words[16]; } b2;
layout(set=0,binding=3,std140) uniform B3 { uvec4 words[16]; } b3;
uvec4 expected(uint base) { return uvec4(0x4b000001u+base*37u)+uvec4(0u,37u,74u,111u); }
void main()
{
    uvec4 differences=uvec4(0u);
    for(uint i=0u;i<16u;++i) {
        differences |= b0.words[i]^expected(i*4u);
        differences |= b1.words[i]^expected(64u+i*4u);
        differences |= b2.words[i]^expected(128u+i*4u);
        differences |= b3.words[i]^expected(192u+i*4u);
    }
    bool bad=any(notEqual(differences,uvec4(0u)));
    for(int i=0;i<3;++i) {
        gl_Position=bad?vec4(2.0,2.0,0.5,1.0):gl_in[i].gl_Position;
        output_quad=input_quad[i];
        EmitVertex();
    }
    EndPrimitive();
}

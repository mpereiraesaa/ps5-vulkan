// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(set=0,binding=7) uniform sampler2D a[24];
layout(set=1,binding=7) uniform sampler2D b[24];
layout(set=2,binding=7) uniform sampler2D c[24];
layout(set=3,binding=7) uniform sampler2D d[24];
layout(location=0) out vec4 vertex_sum;
void main()
{
    const vec2 positions[3]=vec2[3](vec2(-0.7,-0.6),vec2(0.7,-0.6),vec2(0.0,0.7));
    gl_Position=vec4(positions[gl_VertexIndex],0.5,1.0);
    vec4 sum=vec4(0);
    for(int i=0;i<24;++i) {
        sum+=textureLod(a[i],vec2(0.5),0.0)*float(96-i);
        sum+=textureLod(b[i],vec2(0.5),0.0)*float(72-i);
        sum+=textureLod(c[i],vec2(0.5),0.0)*float(48-i);
        sum+=textureLod(d[i],vec2(0.5),0.0)*float(24-i);
    }
    vertex_sum=sum;
}

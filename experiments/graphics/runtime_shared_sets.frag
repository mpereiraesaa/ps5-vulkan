// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(set=0,binding=7) uniform sampler2D a[24];
layout(set=1,binding=7) uniform sampler2D b[24];
layout(set=2,binding=7) uniform sampler2D c[24];
layout(set=3,binding=7) uniform sampler2D d[24];
layout(location=0) in vec4 vertex_sum;
layout(location=0) out vec4 color;
void main()
{
#ifdef VERTEX_SAMPLERS_ONLY
    color=vertex_sum/8192.0;
#else
    vec4 sum=vec4(0);
    for(int i=0;i<24;++i) {
        sum+=textureLod(a[i],vec2(0.5),0.0)*float(1+i);
        sum+=textureLod(b[i],vec2(0.5),0.0)*float(25+i);
        sum+=textureLod(c[i],vec2(0.5),0.0)*float(49+i);
        sum+=textureLod(d[i],vec2(0.5),0.0)*float(73+i);
    }
    color=(sum+2.0*vertex_sum)/32768.0;
#endif
}

// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
/* Mixed-resource consumer workload: every set carries one mandatory uniform
 * buffer at the sparse binding 5 and the 24-element sampler array at binding 7.
 * The uniform block scales that set's whole contribution, so a missing, stale or
 * cross-wired uniform record changes the pixel instead of passing silently. */
layout(set=0,binding=5) uniform Weights0 { vec4 weight; } w0;
layout(set=1,binding=5) uniform Weights1 { vec4 weight; } w1;
layout(set=2,binding=5) uniform Weights2 { vec4 weight; } w2;
layout(set=3,binding=5) uniform Weights3 { vec4 weight; } w3;
layout(set=0,binding=7) uniform sampler2D a[24];
layout(set=1,binding=7) uniform sampler2D b[24];
layout(set=2,binding=7) uniform sampler2D c[24];
layout(set=3,binding=7) uniform sampler2D d[24];
layout(location=0) out vec4 color;
void main()
{
    vec4 sum=vec4(0);
    for(int i=0;i<24;++i) {
        sum+=textureLod(a[i],vec2(0.5),0.0)*(float(1+i)*w0.weight);
        sum+=textureLod(b[i],vec2(0.5),0.0)*(float(25+i)*w1.weight);
        sum+=textureLod(c[i],vec2(0.5),0.0)*(float(49+i)*w2.weight);
        sum+=textureLod(d[i],vec2(0.5),0.0)*(float(73+i)*w3.weight);
    }
    color=sum/65536.0;
}

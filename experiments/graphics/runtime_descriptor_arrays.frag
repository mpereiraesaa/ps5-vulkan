// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(set=0,binding=7) uniform sampler2D a[24];
layout(set=1,binding=7) uniform sampler2D b[24];
layout(set=2,binding=7) uniform sampler2D c[24];
layout(set=3,binding=7) uniform sampler2D d[24];
layout(location=0) out vec4 color;
void main()
{
    vec4 sum=vec4(0.0);
    for(int i=0;i<24;++i)
        sum+=textureLod(a[i],vec2(0.5),0.0)+textureLod(b[i],vec2(0.5),0.0)
            +textureLod(c[i],vec2(0.5),0.0)+textureLod(d[i],vec2(0.5),0.0);
    color=sum/96.0;
}

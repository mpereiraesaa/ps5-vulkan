// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
/* One set, ninety-six sampled descriptors. The existing 96-descriptor witness
 * spreads the same elements over four sets, which bounds the per-stage count but
 * not the per-set count; this fixture puts all of them in a single descriptor
 * set so the per-set capacity is measured directly. Weights and source selection
 * match the four-set workload element for element, so the exact aggregate is the
 * same and the frozen reference values can be compared across both shapes. */
layout(set=0,binding=7) uniform sampler2D a[96];
layout(location=0) out vec4 color;
void main()
{
    vec4 sum=vec4(0);
    for(int i=0;i<96;++i)
        sum+=textureLod(a[i],vec2(0.5),0.0)*float(1+i);
    color=sum/8192.0;
}

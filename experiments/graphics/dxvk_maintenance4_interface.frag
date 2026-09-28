// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
// The two consumed components must survive maintenance4's 4->2 interface.
layout(location=0) flat in uvec2 quad_payload;
layout(location=0) out vec4 color;
void main()
{
    uint q=quad_payload.x;
    if(quad_payload.y!=(q^0x55u)) {
        color=vec4(1.0,0.0,1.0,1.0);
    } else if(q==0u) {
        color=vec4(floor(gl_FragCoord.x)*(4.0/255.0),
                   floor(gl_FragCoord.y)*(4.0/255.0),64.0/255.0,1.0);
    } else if(q==1u) {
        color=vec4(0.0,1.0,0.0,1.0);
    } else {
        color=vec4(0.0,0.0,1.0,1.0);
    }
}

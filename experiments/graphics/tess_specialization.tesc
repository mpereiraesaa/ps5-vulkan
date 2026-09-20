#version 450
layout(vertices=3) out;
layout(constant_id=0) const float channel=0.0;
layout(location=0) in float red[];
layout(location=0) out vec2 red_green[];
void main() {
    red_green[gl_InvocationID]=vec2(red[gl_InvocationID],channel);
    if(gl_InvocationID==0) {
        gl_TessLevelOuter[0]=2;gl_TessLevelOuter[1]=2;
        gl_TessLevelOuter[2]=2;gl_TessLevelOuter[3]=2;
        gl_TessLevelInner[0]=2;gl_TessLevelInner[1]=2;
    }
}

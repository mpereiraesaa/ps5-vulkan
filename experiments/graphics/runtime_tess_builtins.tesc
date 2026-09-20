#version 450
layout(vertices=3) out;
layout(location=0) in vec3 color[];
layout(location=0) out vec3 delivered[];
void main() {
    gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;
    delivered[gl_InvocationID]=color[gl_InvocationID];
    if(gl_InvocationID==0) {
        gl_TessLevelOuter[0]=gl_PatchVerticesIn==3?2:0;
        gl_TessLevelOuter[1]=2;
        gl_TessLevelOuter[2]=2;
        gl_TessLevelInner[0]=1;
    }
}

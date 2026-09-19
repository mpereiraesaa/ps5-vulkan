#version 450
layout(vertices=3) out;
void main() {
    if(gl_InvocationID==0) {
        gl_TessLevelOuter[0]=2;
        gl_TessLevelOuter[1]=4;
    }
}

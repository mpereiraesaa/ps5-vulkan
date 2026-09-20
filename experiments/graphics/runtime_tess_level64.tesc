#version 450
// Own maximum-generation fixture, no SSBO or optional atomics dependency.
layout(vertices=3) out;
void main() {
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0]=64;
        gl_TessLevelOuter[1]=64;
        gl_TessLevelOuter[2]=64;
        gl_TessLevelOuter[3]=64;
        gl_TessLevelInner[0]=64;
        gl_TessLevelInner[1]=64;
    }
}

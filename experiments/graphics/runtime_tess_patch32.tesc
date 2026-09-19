#version 450
layout(vertices=32) out;
layout(location=0) in float identity[];
layout(location=0) out float delivered[];
layout(location=1) patch out float completePatch;
void main() {
    delivered[gl_InvocationID] = identity[gl_InvocationID];
    barrier();
    if (gl_InvocationID == 0) {
        float valid = 1.0;
        for (int i=0; i<32; ++i)
            if (delivered[i] != float(i+1)) valid = 0.0;
        completePatch = valid * 0.5;
        gl_TessLevelOuter[0] = 2.0;
        gl_TessLevelOuter[1] = 2.0;
        gl_TessLevelOuter[2] = 2.0;
        gl_TessLevelInner[0] = 2.0;
    }
}

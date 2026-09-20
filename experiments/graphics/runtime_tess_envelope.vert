#version 450
// Original per-component transport witness. Integer-valued floats are exact.
out gl_PerVertex { vec4 gl_Position; };
#ifdef WITH_PATCH_ENVELOPE
#define ENVELOPE_VECTORS 28
#else
#define ENVELOPE_VECTORS 31
#endif
layout(location=0) out vec4 values[ENVELOPE_VECTORS];
void main() {
    vec2 corners[3]=vec2[3](vec2(-.9,-.9),vec2(.9,-.9),vec2(-.9,.9));
    gl_Position=vec4(corners[gl_VertexIndex],0,1);
    for(int i=0;i<ENVELOPE_VECTORS;i++)
        values[i]=vec4(0,1,2,3)+float(4*i+128*gl_VertexIndex);
}

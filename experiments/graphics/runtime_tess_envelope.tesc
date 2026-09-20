#version 450
// 31*128=3968 per-vertex components: leave room for per-patch factors.
layout(vertices=31) out;
in gl_PerVertex { vec4 gl_Position; } gl_in[];
out gl_PerVertex { vec4 gl_Position; } gl_out[];
#ifdef WITH_PATCH_ENVELOPE
#define ENVELOPE_VECTORS 28
layout(location=28) patch out vec4 patch_values[2];
#else
#define ENVELOPE_VECTORS 31
#endif
layout(location=0) in vec4 values[][ENVELOPE_VECTORS];
layout(location=0) out vec4 delivered[][ENVELOPE_VECTORS];
void main() {
    gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID%3].gl_Position;
    for(int i=0;i<ENVELOPE_VECTORS;i++)
        delivered[gl_InvocationID][i]=values[gl_InvocationID%3][i]+float(512*gl_InvocationID);
#ifdef WITH_PATCH_ENVELOPE
    if(gl_InvocationID<3)
        for(int i=gl_InvocationID;i<2;i+=3)
            patch_values[i]=vec4(0,1,2,3)+float(4*i+128*gl_PrimitiveID);
#endif
    if(gl_InvocationID==0) {
        gl_TessLevelOuter[0]=2;gl_TessLevelOuter[1]=2;
        gl_TessLevelOuter[2]=2;gl_TessLevelInner[0]=2;
    }
}

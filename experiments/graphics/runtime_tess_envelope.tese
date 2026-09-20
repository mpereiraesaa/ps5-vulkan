#version 450
layout(triangles,equal_spacing,cw) in;
in gl_PerVertex { vec4 gl_Position; } gl_in[];
out gl_PerVertex { vec4 gl_Position; };
#ifdef WITH_PATCH_ENVELOPE
#define ENVELOPE_VECTORS 28
layout(location=28) patch in vec4 patch_values[2];
#else
#define ENVELOPE_VECTORS 31
#endif
layout(location=0) in vec4 delivered[][ENVELOPE_VECTORS];
layout(location=0) out vec3 out_color;
void main() {
    bool valid=true;
    for(int v=0;v<31;v++) for(int i=0;i<ENVELOPE_VECTORS;i++) {
        vec4 expected=vec4(0,1,2,3)+float(4*i+128*(v%3)+512*v);
        if(any(notEqual(delivered[v][i],expected)))valid=false;
    }
#ifdef WITH_PATCH_ENVELOPE
    for(int i=0;i<2;i++) {
        vec4 expected=vec4(0,1,2,3)+float(4*i+128*gl_PrimitiveID);
        if(any(notEqual(patch_values[i],expected)))valid=false;
    }
#endif
    gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+
                gl_TessCoord.y*gl_in[1].gl_Position+
                gl_TessCoord.z*gl_in[2].gl_Position;
    out_color=valid?vec3(gl_TessCoord.y,gl_TessCoord.z,.5):vec3(1,0,1);
}

#version 450
layout(triangles,equal_spacing,cw) in;
layout(location=0) in vec3 delivered[];
layout(location=0) out vec3 color;
void main() {
    gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+
                gl_TessCoord.y*gl_in[1].gl_Position+
                gl_TessCoord.z*gl_in[2].gl_Position;
    bool ok=gl_PatchVerticesIn==3 && gl_PrimitiveID==0 &&
            gl_TessLevelOuter[0]==2 && gl_TessLevelInner[0]==1;
    color=ok?delivered[0]:vec3(0);
}

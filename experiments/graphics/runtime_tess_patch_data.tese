#version 450
layout(triangles,equal_spacing,cw) in;
layout(location=0) in vec3 delivered[];
layout(location=2) patch in float patch_blue;
layout(location=0) out vec3 out_color;
void main() {
    gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+
                gl_TessCoord.y*gl_in[1].gl_Position+
                gl_TessCoord.z*gl_in[2].gl_Position;
    vec3 interpolated=gl_TessCoord.x*delivered[0]+gl_TessCoord.y*delivered[1]+
                      gl_TessCoord.z*delivered[2];
    out_color=vec3(interpolated.xy,patch_blue);
}

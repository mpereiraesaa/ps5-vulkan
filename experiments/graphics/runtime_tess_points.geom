#version 450
// Own TES->GS witness: move every domain point four pixels right/up on 64x64
// and permute its colour. Bypassing GS cannot satisfy the pixel oracle.
layout(points) in;
layout(points, max_vertices=1) out;
layout(location=0) in vec3 domain_color[];
layout(location=0) out vec3 out_color;
void main() {
    gl_Position=gl_in[0].gl_Position+vec4(0.125,-0.125,0,0);
    out_color=domain_color[0].gbr;
    EmitVertex();
    EndPrimitive();
}

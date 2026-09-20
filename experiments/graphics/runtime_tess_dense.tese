#version 450
layout(quads,equal_spacing,cw) in;
layout(location=0) out vec4 out_color;
void main() {
    gl_Position=vec4(gl_TessCoord.xy*1.8-0.9,0,1);
    out_color=vec4(gl_TessCoord.xy,0.5,0.25);
}

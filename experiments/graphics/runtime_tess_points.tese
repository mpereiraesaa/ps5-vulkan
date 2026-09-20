#version 450
// Level-two quad: nine domain points, placed at pixel centres on a 64x64 target.
// Point size is the Vulkan default 1; no optional point-size feature is needed.
layout(quads, equal_spacing, cw, point_mode) in;
layout(location=0) out vec3 out_color;
void main() {
    gl_Position=vec4(gl_TessCoord.xy*1.5-0.734375,0,1);
    out_color=vec3(gl_TessCoord.xy,0.5);
}

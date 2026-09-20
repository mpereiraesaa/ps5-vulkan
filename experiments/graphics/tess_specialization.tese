#version 450
layout(quads,equal_spacing,cw,point_mode) in;
layout(constant_id=0) const float channel=0.0;
layout(location=0) in vec2 red_green[];
layout(location=0) out vec3 out_color;
void main() {
    gl_Position=vec4(gl_TessCoord.xy*1.5-0.734375,0,1);
    out_color=vec3(red_green[0],channel);
}

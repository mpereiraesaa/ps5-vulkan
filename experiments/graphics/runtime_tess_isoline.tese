#version 450
layout(isolines,equal_spacing) in;
layout(location=0) out vec3 out_color;
void main() {
    // For the 64x64 witness: x=8.5..56.5, y=16.5 or48.5.
    gl_Position=vec4(gl_TessCoord.x*1.5-0.734375,
                     gl_TessCoord.y*2.0-0.484375,0,1);
    out_color=vec3(gl_TessCoord.xy,0.5);
}

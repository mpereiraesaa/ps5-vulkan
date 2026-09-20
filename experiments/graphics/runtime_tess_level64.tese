#version 450
// Pack the 65 bottom-edge domain points into an 8-column atlas on 64x64.
// Missing subdivision levels produce missing pixels, unlike a flat triangle.
layout(quads, equal_spacing, cw, point_mode) in;
layout(location=0) out vec3 out_color;
void main() {
    out_color=vec3(1,0,0);
    if (gl_TessCoord.y != 0.0) {
        gl_Position=vec4(2,2,0,1);
        return;
    }
    uint index=uint(gl_TessCoord.x*64.0+0.5);
    vec2 pixel=vec2(index%8u,index/8u)*6.0+7.5;
    gl_Position=vec4(pixel/32.0-1.0,0,1);
}

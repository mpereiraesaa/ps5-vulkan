#version 450
layout(triangles, equal_spacing, cw) in;
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[8]; };
void main() {
    gl_Position=vec4(gl_TessCoord.x*1.8-0.9,gl_TessCoord.y*1.8-0.9,0,1);
    for(int i=0;i<8;i++) gl_CullDistance[i]=1.0;
    gl_CullDistance[4]=gl_TessCoord.x;
    gl_CullDistance[5]=gl_TessCoord.y;
    gl_CullDistance[6]=0.5;
}

#version 450
layout(triangles, equal_spacing, cw) in;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[3]; float gl_CullDistance[4]; };
void main() {
    gl_Position=vec4(gl_TessCoord.x*1.8-0.9,gl_TessCoord.y*1.8-0.9,0,1);
    for(int i=0;i<3;i++) gl_ClipDistance[i]=1.0;
    gl_CullDistance[0]=gl_TessCoord.x;
    gl_CullDistance[1]=gl_TessCoord.y;
    gl_CullDistance[2]=0.5;
    gl_CullDistance[3]=1.0;
}

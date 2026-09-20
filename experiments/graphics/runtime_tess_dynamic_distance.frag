#version 450
in float gl_CullDistance[4];
layout(location=0) out vec4 color;
void main() {
    int index=int(gl_CullDistance[0]*4.0)&3;
    color=vec4(gl_CullDistance[index],0,0,1);
}

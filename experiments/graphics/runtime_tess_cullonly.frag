#version 450
in float gl_CullDistance[8];
layout(location=0) out vec4 color;
void main() {
    color=vec4(gl_CullDistance[4],gl_CullDistance[5],gl_CullDistance[6],1);
}

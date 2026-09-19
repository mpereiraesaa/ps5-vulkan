#version 450
// Deliberately matches the independent coordinate control's screen triangle.
layout(location=0) out vec3 color;
void main() {
    vec2 uv[3]=vec2[3](vec2(0,0),vec2(1,0),vec2(0,1));
    gl_Position=vec4(uv[gl_VertexIndex]*1.8-0.9,0,1);
    color=vec3(uv[gl_VertexIndex],0.5);
}

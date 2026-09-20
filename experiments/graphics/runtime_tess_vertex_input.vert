#version 450
layout(location=0) in vec2 position;
layout(location=0) out vec3 color;
void main() {
    gl_Position=vec4(position*1.8-0.9,0,1);
    color=vec3(position,0.5);
}

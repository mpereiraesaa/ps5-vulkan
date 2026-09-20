#version 450
layout(constant_id=0) const float channel=0.0;
layout(location=0) out float red;
void main() {
    gl_Position=vec4(0,0,0,1);
    red=channel;
}

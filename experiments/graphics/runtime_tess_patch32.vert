#version 450
layout(location=0) out float identity;
void main() {
    identity = float(gl_VertexIndex + 1);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}

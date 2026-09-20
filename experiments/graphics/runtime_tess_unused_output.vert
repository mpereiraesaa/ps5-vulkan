#version 450
layout(location=0) out float identity;
layout(location=7) out vec4 unusedByControl;
void main() {
    identity = float(gl_VertexIndex + 1);
    unusedByControl = vec4(identity, 2.0, 3.0, 4.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}

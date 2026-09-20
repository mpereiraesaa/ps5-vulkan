#version 450
layout(triangles, equal_spacing, ccw) in;
layout(location=1) patch in float completePatch;
layout(location=0) out vec3 color;
void main() {
    vec2 uv = gl_TessCoord.xy;
    gl_Position = vec4(uv*1.8-0.9, 0.0, 1.0);
    color = vec3(uv, completePatch);
}

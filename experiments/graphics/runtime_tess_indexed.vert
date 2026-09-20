#version 450
layout(location=0) out vec3 color;
void main() {
    // UINT16 indices {4,1,6} plus baseVertex=1 must reach LS as {5,2,7}.
    // Ignored binding offset/firstIndex reads sentinels; ignored indices or
    // baseVertex cannot produce the three expected control points.
    vec2 uv=vec2(4.0);
    if(gl_VertexIndex==5)uv=vec2(0,0);
    if(gl_VertexIndex==2)uv=vec2(1,0);
    if(gl_VertexIndex==7)uv=vec2(0,1);
    gl_Position=vec4(uv*1.8-0.9,0,1);
    color=vec3(uv,0.5);
}

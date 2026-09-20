#version 450
layout(location=0) out vec3 color;
void main() {
    vec2 uv[3]=vec2[3](vec2(0,0),vec2(1,0),vec2(0,1));
    int patch_id=gl_VertexIndex/3;
    vec2 p=uv[gl_VertexIndex%3];
    gl_Position=vec4(p.x*0.8-0.9+float(patch_id),p.y*1.8-0.9,0,1);
    color=vec3(p,patch_id==0?0.25:0.75);
}

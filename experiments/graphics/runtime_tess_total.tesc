#version 450
// 32*(120 user + 1 scalar + 4 Position) + 90 patch + 6 factors = 4096.
// Before native use, Patch locations must be rewritten into their independent
// namespace; high source locations avoid GLSL frontend collisions.
layout(vertices=32) out;
in gl_PerVertex { vec4 gl_Position; } gl_in[];
out gl_PerVertex { vec4 gl_Position; } gl_out[];
layout(location=0) out vec4 values[][30];
layout(location=30) out float tail_value[];
layout(location=31) patch out vec4 patch_values[22];
layout(location=53) patch out vec2 patch_tail;
void main() {
    int v=gl_InvocationID;
    gl_out[gl_InvocationID].gl_Position=gl_in[v%3].gl_Position;
    for(int i=0;i<30;i++)
        values[gl_InvocationID][i]=vec4(0,1,2,3)+float(128*v+4*i);
    tail_value[gl_InvocationID]=float(128*v+120);
    if(v==0) {
        for(int i=0;i<22;i++)patch_values[i]=vec4(0,1,2,3)+float(8192+4*i);
        patch_tail=vec2(8280,8281);
        gl_TessLevelOuter[0]=2;gl_TessLevelOuter[1]=2;
        gl_TessLevelOuter[2]=2;gl_TessLevelOuter[3]=2;
        gl_TessLevelInner[0]=2;gl_TessLevelInner[1]=2;
    }
}

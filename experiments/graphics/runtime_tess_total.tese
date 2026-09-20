#version 450
layout(triangles,equal_spacing,cw) in;
in gl_PerVertex { vec4 gl_Position; } gl_in[];
out gl_PerVertex { vec4 gl_Position; };
layout(location=0) in vec4 values[][30];
layout(location=30) in float tail_value[];
layout(location=31) patch in vec4 patch_values[22];
layout(location=53) patch in vec2 patch_tail;
layout(location=0) out vec3 out_color;
void main() {
    bool valid=true;
    vec2 corners[3]=vec2[3](vec2(-.9,-.9),vec2(.9,-.9),vec2(-.9,.9));
    for(int v=0;v<32;v++) {
        for(int i=0;i<30;i++)
            if(any(notEqual(values[v][i],vec4(0,1,2,3)+float(128*v+4*i))))valid=false;
        if(tail_value[v]!=float(128*v+120))valid=false;
        if(any(notEqual(gl_in[v].gl_Position,vec4(corners[v%3],0,1))))valid=false;
    }
    for(int i=0;i<22;i++)
        if(any(notEqual(patch_values[i],vec4(0,1,2,3)+float(8192+4*i))))valid=false;
    if(any(notEqual(patch_tail,vec2(8280,8281))))valid=false;
    for(int i=0;i<4;i++)if(gl_TessLevelOuter[i]!=2)valid=false;
    for(int i=0;i<2;i++)if(gl_TessLevelInner[i]!=2)valid=false;
    gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+
                gl_TessCoord.y*gl_in[1].gl_Position+
                gl_TessCoord.z*gl_in[2].gl_Position;
    out_color=valid?vec3(gl_TessCoord.y,gl_TessCoord.z,.5):vec3(1,0,1);
}

#version 450
layout(triangles,equal_spacing,cw) in;
layout(location=0) patch in vec4 patch_values[30];
layout(location=0) out vec3 out_color;
void main() {
    bool valid=true;
    for(int i=0;i<30;i++) {
        vec4 expected=vec4(0,1,2,3)+float(4*i+128*gl_PrimitiveID);
        if(any(notEqual(patch_values[i],expected)))valid=false;
    }
    gl_Position=vec4(gl_TessCoord.xy*1.8-.9,0,1);
    out_color=valid?vec3(gl_TessCoord.xy,.5):vec3(1,0,1);
}

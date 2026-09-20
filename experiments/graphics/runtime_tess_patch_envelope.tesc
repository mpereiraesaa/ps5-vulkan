#version 450
layout(vertices=3) out;
layout(location=0) patch out vec4 patch_values[30];
void main() {
    // Three disjoint writers: every patch component is observable in TES.
    for(int i=gl_InvocationID;i<30;i+=3)
        patch_values[i]=vec4(0,1,2,3)+float(4*i+128*gl_PrimitiveID);
    if(gl_InvocationID==0) {
        gl_TessLevelOuter[0]=2;gl_TessLevelOuter[1]=2;
        gl_TessLevelOuter[2]=2;gl_TessLevelInner[0]=2;
    }
}

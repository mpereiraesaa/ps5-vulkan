#version 450
// The ordinary witness image, with coefficients and guards supplied inline.
layout(location=0) flat in uint quad;
layout(location=0) out vec4 color;
layout(set=0,binding=0,std140) uniform First { uvec4 values; uint guard; } first;
layout(set=0,binding=1,std140) uniform Second { uint guard; } second;
void main()
{
    if(first.guard!=0x13579bdfu || second.guard!=0x2468ace0u) {
        color=vec4(1.0,0.0,1.0,1.0);
    } else if(quad==0u) {
        color=vec4(floor(gl_FragCoord.x)*(float(first.values.x)/255.0),
                   floor(gl_FragCoord.y)*(float(first.values.y)/255.0),
                   float(first.values.z)/255.0,float(first.values.w)/255.0);
    } else if(quad==1u) {
        color=vec4(0.0,float(first.values.w)/255.0,0.0,float(first.values.w)/255.0);
    } else {
        color=vec4(0.0,0.0,float(first.values.w)/255.0,float(first.values.w)/255.0);
    }
}

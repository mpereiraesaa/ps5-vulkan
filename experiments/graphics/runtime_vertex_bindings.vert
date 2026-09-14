#version 450
layout(constant_id=0) const bool all_inputs=true;
layout(location=0) in vec4 input0;
layout(location=1) in vec4 input1;
layout(location=2) in vec4 input2;
layout(location=3) in vec4 input3;
layout(location=4) in vec4 input4;
layout(location=5) in vec4 input5;
layout(location=6) in vec4 input6;
layout(location=7) in vec4 input7;
layout(location=8) in vec4 input8;
layout(location=9) in vec4 input9;
layout(location=10) in vec4 input10;
layout(location=11) in vec4 input11;
layout(location=12) in vec4 input12;
layout(location=13) in vec4 input13;
layout(location=14) in vec4 input14;
layout(location=15) in vec4 input15;
layout(location=0) out vec3 color;
void main() {
    vec4 position=input0;
    if(all_inputs) {
        position+=input1;
        position+=input2;
        position+=input3;
        position+=input4;
        position+=input5;
        position+=input6;
        position+=input7;
        position+=input8;
        position+=input9;
        position+=input10;
        position+=input11;
        position+=input12;
        position+=input13;
        position+=input14;
        position+=input15;
    }
    gl_Position=position;
    color=vec3(0.25,0.5,0.75);
}

#version 450
// Owned binding-order oracle: locations map to reversed binding numbers.
layout(constant_id = 0) const uint mode = 0;
layout(location = 0) out vec4 component_validity;
layout(location = 0) in vec4 value0;
layout(location = 1) in vec4 value1;
layout(location = 2) in vec4 value2;
layout(location = 3) in vec4 value3;
layout(location = 4) in vec4 value4;
layout(location = 5) in vec4 value5;
layout(location = 6) in vec4 value6;
layout(location = 7) in vec4 value7;
layout(location = 8) in vec4 value8;
layout(location = 9) in vec4 value9;
layout(location = 10) in vec4 value10;
layout(location = 11) in vec4 value11;
layout(location = 12) in vec4 value12;
layout(location = 13) in vec4 value13;
layout(location = 14) in vec4 value14;
layout(location = 15) in vec4 value15;
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-0.7, -0.6), vec2(0.7, -0.6), vec2(0.0, 0.7));
    gl_Position = vec4(positions[gl_VertexIndex % 3], 0.5, 1.0);
    bool valid = all(equal(value0, vec4(16, 17, 18, 19)));
    if (mode == 0) valid = valid && all(equal(value1, vec4(15, 16, 17, 18)));
    if (mode == 0) valid = valid && all(equal(value2, vec4(14, 15, 16, 17)));
    if (mode == 0) valid = valid && all(equal(value3, vec4(13, 14, 15, 16)));
    if (mode == 0) valid = valid && all(equal(value4, vec4(12, 13, 14, 15)));
    if (mode == 0) valid = valid && all(equal(value5, vec4(11, 12, 13, 14)));
    if (mode == 0) valid = valid && all(equal(value6, vec4(10, 11, 12, 13)));
    if (mode == 0) valid = valid && all(equal(value7, vec4(9, 10, 11, 12)));
    if (mode == 0) valid = valid && all(equal(value8, vec4(8, 9, 10, 11)));
    if (mode == 0) valid = valid && all(equal(value9, vec4(7, 8, 9, 10)));
    if (mode == 0) valid = valid && all(equal(value10, vec4(6, 7, 8, 9)));
    if (mode == 0) valid = valid && all(equal(value11, vec4(5, 6, 7, 8)));
    if (mode != 1) valid = valid && all(equal(value12, vec4(4, 5, 6, 7)));
    if (mode == 0) valid = valid && all(equal(value13, vec4(3, 4, 5, 6)));
    if (mode == 0) valid = valid && all(equal(value14, vec4(2, 3, 4, 5)));
    if (mode == 0) valid = valid && all(equal(value15, vec4(1, 2, 3, 4)));
    component_validity = valid ? vec4(1.0) : vec4(1.0, 0.0, 0.0, 1.0);
}

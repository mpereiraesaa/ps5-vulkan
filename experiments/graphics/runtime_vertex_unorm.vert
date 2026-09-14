#version 450
layout(location = 0) in vec4 attribute_value;
layout(location = 0) out vec4 component_validity;
layout(constant_id = 0) const float expected_r = 0.0;
layout(constant_id = 1) const float expected_g = 0.0;
layout(constant_id = 2) const float expected_b = 0.0;
layout(constant_id = 3) const float expected_a = 1.0;
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-0.7, -0.6), vec2(0.7, -0.6), vec2(0.0, 0.7));
    const vec4 expected = vec4(expected_r, expected_g, expected_b, expected_a);
    const vec4 error = abs(attribute_value - expected);
    gl_Position = vec4(positions[gl_VertexIndex % 3], 0.5, 1.0);
    component_validity = vec4(error.x < 0.000001,
                              error.y < 0.000001,
                              error.z < 0.000001,
                              error.w < 0.000001);
}

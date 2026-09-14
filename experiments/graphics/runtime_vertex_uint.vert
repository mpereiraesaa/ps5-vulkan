#version 450
layout(location = 0) in uvec4 attribute_value;
layout(location = 0) out vec4 component_validity;
layout(constant_id = 0) const uint expected_r = 0u;
layout(constant_id = 1) const uint expected_g = 0u;
layout(constant_id = 2) const uint expected_b = 0u;
layout(constant_id = 3) const uint expected_a = 1u;
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-0.7, -0.6), vec2(0.7, -0.6), vec2(0.0, 0.7));
    gl_Position = vec4(positions[gl_VertexIndex % 3], 0.5, 1.0);
    component_validity = vec4(
        attribute_value.x == expected_r,
        attribute_value.y == expected_g,
        attribute_value.z == expected_b,
        attribute_value.w == expected_a);
}

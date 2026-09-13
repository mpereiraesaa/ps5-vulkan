#version 450
layout(location = 0) in ivec4 attribute_value;
layout(location = 0) out vec4 component_validity;
layout(constant_id = 0) const int component_count = 4;
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-0.7, -0.6), vec2(0.7, -0.6), vec2(0.0, 0.7));
    gl_Position = vec4(positions[gl_VertexIndex % 3], 0.5, 1.0);
    component_validity = vec4(
        attribute_value.x == -1,
        component_count >= 2 ? attribute_value.y == -1 : attribute_value.y == 0,
        component_count >= 3 ? attribute_value.z == -1 : attribute_value.z == 0,
        component_count >= 4 ? attribute_value.w == -1 : attribute_value.w == 1);
}

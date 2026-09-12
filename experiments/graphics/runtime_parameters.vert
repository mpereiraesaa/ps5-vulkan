#version 450
layout(constant_id = 0) const float scale = 1.0;
layout(push_constant) uniform PushConstants {
    vec2 offset;
    float brightness;
} push_data;
layout(location = 0) out vec3 color;
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-0.7, -0.6), vec2(0.7, -0.6), vec2(0.0, 0.7));
    const vec3 colors[3] = vec3[3](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));
    gl_Position = vec4(positions[gl_VertexIndex] * scale + push_data.offset, 0.5, 1.0);
    color = colors[gl_VertexIndex];
}

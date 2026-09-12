#version 450
layout(constant_id = 1) const float intensity = 1.0;
layout(push_constant) uniform PushConstants {
    vec2 offset;
    float brightness;
} push_data;
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 output_color;
void main()
{
    output_color = vec4(color * intensity * push_data.brightness, 1.0);
}

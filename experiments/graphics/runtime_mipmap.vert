#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 texcoord;
layout(location = 0) out vec2 uv;
void main()
{
    float c = cos(texcoord.z), s = sin(texcoord.z);
    vec3 p = vec3(c * position.x + s * position.z, position.y,
                  -s * position.x + c * position.z);
    float cx = 0.939372713, sx = 0.342897807;
    p = vec3(p.x, cx * p.y - sx * p.z, sx * p.y + cx * p.z);
    float distance = 4.0 - p.z;
    float near_plane = 0.1, far_plane = 20.0;
    gl_Position = vec4(p.x * 1.2, p.y * 2.133333333,
        far_plane / (far_plane - near_plane) * distance -
        far_plane * near_plane / (far_plane - near_plane), distance);
    uv = texcoord.xy;
}

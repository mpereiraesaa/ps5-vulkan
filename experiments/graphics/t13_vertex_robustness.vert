#version 450
/* T13 vertex robustness witness: one full-target triangle whose colour is the
 * binding-0 attribute, passed flat so every covered pixel shows the value the
 * provoking vertex fetched. */
layout(location = 0) in vec4 attribute_value;
layout(location = 0) flat out vec4 color;
void main()
{
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex % 3], 0.0, 1.0);
    color = attribute_value;
}

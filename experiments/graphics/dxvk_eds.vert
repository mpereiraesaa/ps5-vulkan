#version 450
/* EDS witness: the position and the colour both come from binding 0, whose
 * stride the draw supplies dynamically; the colour is passed flat. */
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 attribute_color;
layout(location = 0) flat out vec4 color;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    color = attribute_color;
}

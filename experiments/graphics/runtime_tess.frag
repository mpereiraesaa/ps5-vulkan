#version 450
/* The fragment stage every fixture pairs with: one colour output, no input, so
 * a rejection can never come from the varying interface. */
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(color, 1.0);
}

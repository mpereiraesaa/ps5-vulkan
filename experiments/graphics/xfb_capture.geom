#version 450
/* The pinned DXVK stream-output shape: a pass-through geometry stage that
 * takes points and emits one point per input, capturing two vec4 elements of
 * buffer 0 at offsets 0 and 16 with a 32-byte stride, on stream 0. */
layout(points) in;
layout(points, max_vertices = 1) out;
layout(location = 0) in vec4 value[];
layout(location = 0, xfb_buffer = 0, xfb_stride = 32, xfb_offset = 0) out vec4 first;
layout(location = 1, xfb_buffer = 0, xfb_offset = 16) out vec4 second;
void main()
{
    first = value[0];
    second = value[0].wzyx;
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
}

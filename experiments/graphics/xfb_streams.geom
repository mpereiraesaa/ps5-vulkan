#version 450
/* geometryStreams: stream 0 captures into buffer 0 and stream 1 into buffer 1,
 * both from one point per input, as a D3D11 multi-stream declaration does. */
layout(points) in;
layout(points, max_vertices = 2) out;
layout(location = 0) in vec4 value[];
layout(stream = 0, location = 0, xfb_buffer = 0, xfb_stride = 16, xfb_offset = 0) out vec4 zero;
layout(stream = 1, location = 1, xfb_buffer = 1, xfb_stride = 16, xfb_offset = 0) out vec4 one;
void main()
{
    zero = value[0];
    gl_Position = gl_in[0].gl_Position;
    EmitStreamVertex(0);
    one = value[0].yxwz;
    EmitStreamVertex(1);
}

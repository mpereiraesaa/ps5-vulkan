#version 450
/* Lines in: the other input family a geometryShader device is expected to
 * accept. The stage reads BOTH vertices of its input primitive - a stage that
 * only ever read gl_in[0] would emit the same marker for every line of a strip
 * - and emits a quad centred on the segment whose size comes from the segment
 * itself, coloured by the mean of the two endpoint colours. The oracle knows
 * the segment endpoints, so a correct read, a read of only the first vertex and
 * a zero read produce three different images. */
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec3 color[];
layout(location = 0) out vec3 out_color;
void main()
{
    vec2 a = gl_in[0].gl_Position.xy;
    vec2 b = gl_in[1].gl_Position.xy;
    vec2 centre = (a + b) * 0.5;
    /* Half the segment length plus a fixed margin, so the marker grows with the
     * input the stage actually read. */
    vec2 extent = abs(b - a) * 0.5 + vec2(0.1, 0.1);
    const vec2 signs[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0),
                                  vec2(-1.0, 1.0), vec2(1.0, 1.0));
    vec3 mean = (color[0] + color[1]) * 0.5;
    for (int i = 0; i < 4; ++i) {
        gl_Position = vec4(centre + signs[i] * extent, 0.5, 1.0);
        out_color = mean;
        EmitVertex();
    }
    EndPrimitive();
}

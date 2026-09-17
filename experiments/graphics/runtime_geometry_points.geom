#version 450
/* Points in: the input family a device that advertises geometryShader is
 * expected to feed the stage, and the one the pinned conformance module uses
 * for its point-list geometry pipelines. The stage reads the single input
 * vertex of its primitive - gl_in[0] is the whole input, not a third of it -
 * and emits one axis-aligned quad per input point whose PLACE and COLOUR both
 * come from that point. A stage that read a fixed item or a shifted one lands
 * somewhere the oracle does not expect, which is what makes this a witness. */
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec3 color[];
layout(location = 0) out vec3 out_color;
void main()
{
    vec2 centre = gl_in[0].gl_Position.xy;
    /* Small enough that the markers of the witness's point list do not touch
     * each other, so each one is an independently verifiable place. */
    const vec2 offsets[4] = vec2[4](vec2(-0.12, -0.12), vec2(0.12, -0.12),
                                    vec2(-0.12, 0.12), vec2(0.12, 0.12));
    for (int i = 0; i < 4; ++i) {
        gl_Position = vec4(centre + offsets[i], 0.5, 1.0);
        out_color = color[0];
        EmitVertex();
    }
    EndPrimitive();
}

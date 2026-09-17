#version 450
/* The geometry witness itself. Every mode emits the same three-vertex strip for
 * the primitives it keeps, so the only difference between modes is which
 * primitives survive, where their vertices go and what varying they carry. */
layout(triangles) in;
/* Nine vertices: the amplification mode emits three sub-triangles for every
 * input primitive, which is the real test that one invocation can produce more
 * geometry than it received. */
layout(triangle_strip, max_vertices = 9) out;
layout(constant_id = 0) const int MODE = 0;
layout(location = 0) in vec3 color[];
layout(location = 0) out vec3 out_color;
void main()
{
    /* 0 passthrough, 1 shrink, 2 suppress, 3 rewrite the varying, 4 amplify by
     * splitting every input triangle at its centroid. The
     * per-primitive input (gl_PrimitiveIDIn) is not part of this witness yet:
     * the interface policy refuses built-ins the adapter cannot deliver, and
     * this stage's per-primitive delivery has not been established. */
    if (MODE == 2) { return; }
    if (MODE == 6) {
        /* Positions from the input, colour constant: this is the position half
         * of the ES->GS handoff on its own, so a wrong varying cannot hide a
         * correct position path or the other way round. */
        for (int i = 0; i < gl_in.length(); i) {
            gl_Position = gl_in[i].gl_Position;
            out_color = vec3(1.0, 1.0, 1.0);
            EmitVertex();
        }
        EndPrimitive();
        return;
    }
    if (MODE == 5) {
        /* Input-independent emission: a fixed centred quad with a fixed
         * colour, so "the stage runs and its output reaches the pixel stage" is
         * separable from "the stage receives the vertex data it was given". */
        const vec2 corners[4] = vec2[4](vec2(-0.5, -0.5), vec2(0.5, -0.5),
                                        vec2(-0.5, 0.5), vec2(0.5, 0.5));
        for (int i = 0; i < 4; ++i) {
            gl_Position = vec4(corners[i], 0.5, 1.0);
            out_color = vec3(0.25, 0.5, 0.75);
            EmitVertex();
        }
        EndPrimitive();
        return;
    }
    if (MODE == 4) {
        vec4 middle = (gl_in[0].gl_Position + gl_in[1].gl_Position +
                         gl_in[2].gl_Position) / 3.0;
        for (int corner = 0; corner < 3; ++corner) {
            const int order[3] = int[3](corner, (corner + 1) % 3, (corner + 2) % 3);
            gl_Position = gl_in[order[0]].gl_Position;
            out_color = color[order[0]];
            EmitVertex();
            gl_Position = gl_in[order[1]].gl_Position;
            out_color = color[order[1]];
            EmitVertex();
            gl_Position = middle;
            out_color = (color[0] + color[1] + color[2]) / 3.0;
            EmitVertex();
            EndPrimitive();
        }
        return;
    }
    for (int i = 0; i < gl_in.length(); ++i) {
        vec4 position = gl_in[i].gl_Position;
        vec3 value = color[i];
        if (MODE == 1) { position = vec4(position.xy * 0.5, position.zw); }
        if (MODE == 3) { value = vec3(1.0 - value.x, value.y, value.z); }
        gl_Position = position;
        out_color = value;
        EmitVertex();
    }
    EndPrimitive();
}

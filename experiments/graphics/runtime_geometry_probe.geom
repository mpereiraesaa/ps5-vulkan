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
    if (MODE == 11) {
        /* The discriminating read diagnostic: ONE fixed index, and the emitted
         * marker carries both the place and the colour of the value that read
         * returned. The oracle expects both markers (one per input primitive)
         * at their computable places with those exact colours, so the outcome
         * names which way it failed instead of only "an unstable image":
         * correct read - both markers exactly right; read of another item - a
         * marker at a computable other place with that item's colour; a clean
         * target - the geometry half did not run for this shape; one marker -
         * an input primitive was not processed at all. The position is clamped
         * into the target so the marker cannot fall off the edge the way the
         * extended input triangle does, while the colour uses the UNCLAMPED
         * value so a different read is still visible. */
        vec2 p = gl_in[0].gl_Position.xy;
        vec2 q = vec2(clamp(p.x, -0.75, 0.75), clamp(p.y, -0.75, 0.75));
        /* An axis-aligned QUAD, not a triangle: its edges are parallel to the
         * target's axes, so the oracle's per-pixel test and the rasterizer's
         * pixel-centre rule agree exactly and a correct image cannot fail on a
         * fill rule. The two markers are 12x12 pixels at 64x64 by construction. */
        const vec2 offsets[4] = vec2[4](vec2(-0.2, -0.2), vec2(0.2, -0.2),
                                        vec2(-0.2, 0.2), vec2(0.2, 0.2));
        for (int i = 0; i < 4; ++i) {
            gl_Position = vec4(q + offsets[i], 0.5, 1.0);
            out_color = vec3(p.x * 0.5 + 0.5, p.y * 0.5 + 0.5, 0.25);
            EmitVertex();
        }
        EndPrimitive();
        return;
    }
    if (MODE >= 12 && MODE <= 14) {
        /* The read-value readback, one mode per input vertex: ONE scalar read
         * with a fixed index, written back as its raw IEEE-754 bytes so the
         * oracle asserts the exact 32-bit pattern that was at the address the
         * stage read, not a colour derived from it. The place IS the value read
         * (clamped into the target), and the readback's own vertex stage gives
         * every item a different value, so a read that returned another item
         * lands at that item's own place carrying that item's own bytes - the
         * image names the item that was read. The lower quadrant carries the low
         * three bytes in its colour and the upper one the high byte in red with
         * fixed green/blue markers, so a zero high byte is still visible ink, and
         * the quadrants are small enough for the per-item places not to overlap. */
        const int index = MODE - 12;
        float value = index == 0 ? gl_in[0].gl_Position.x
                    : index == 1 ? gl_in[1].gl_Position.x
                                 : gl_in[2].gl_Position.x;
        uint bits = floatBitsToUint(value);
        float cx = clamp(value, -0.75, 0.75);
        /* Narrow in x: the readback's own input values are 0.0625 apart, so a
         * half extent above that would let neighbouring items overlap. */
        const vec2 offsets[4] = vec2[4](vec2(-0.03, -0.2), vec2(0.03, -0.2),
                                        vec2(-0.03, 0.2), vec2(0.03, 0.2));
        for (int row = 0; row < 2; ++row) {
            float cy = row == 0 ? -0.5 : 0.5;
            vec3 colour = row == 0
                ? vec3(float((bits >> 16) & 0xffu), float((bits >> 8) & 0xffu),
                       float(bits & 0xffu)) / 255.0
                : vec3(float((bits >> 24) & 0xffu) / 255.0, 128.0 / 255.0, 64.0 / 255.0);
            for (int i = 0; i < 4; ++i) {
                gl_Position = vec4(cx + offsets[i].x, cy + offsets[i].y, 0.5, 1.0);
                out_color = colour;
                EmitVertex();
            }
            EndPrimitive();
        }
        return;
    }
    if (MODE == 6) {
        /* Positions from the input, colour constant: this is the position half
         * of the ES->GS handoff on its own, so a wrong varying cannot hide a
         * correct position path or the other way round. */
        /* `++i`: without it this loop never terminates, keeps emitting past the
         * stage's maximum vertex count and loses the device - which is what this
         * case did for several sessions, misread as a hardware or addressing
         * fault. It is a witness bug: the shared loop below has always had it. */
        for (int i = 0; i < gl_in.length(); ++i) {
            gl_Position = gl_in[i].gl_Position;
            out_color = vec3(1.0, 1.0, 1.0);
            EmitVertex();
        }
        EndPrimitive();
        return;
    }
    if (MODE == 10) {
        /* Sentinel: real coverage with an observable colour. The vertex's read
         * position drives the colour (position in [-1,1] mapped to [0,1]), so a
         * correct read, a zero read and a shifted read produce three different
         * images; the oracle asserts the exact per-pixel colour derived from the
         * same mapping, which is the coverage-plus-value split the other input
         * cases lack: the positions case asserts coverage only, and the
         * passthrough case asserts a value it cannot obtain yet.
         * LIMIT: the colour is affine in the read position and the input is two
         * structurally identical triangles, so exchanging the two primitives'
         * items wholesale maps the image onto itself; the sentinel separates a
         * correct read from zero, garbage and shifted items, not from that
         * exchange. Pair it with the passthrough case (which reads the varying
         * and fails) to separate the position path from the varying path. */
        for (int i = 0; i < 3; ++i) {
            gl_Position = gl_in[i].gl_Position;
            out_color = vec3(gl_in[i].gl_Position.x * 0.5 + 0.5,
                             gl_in[i].gl_Position.y * 0.5 + 0.5,
                             0.25);
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

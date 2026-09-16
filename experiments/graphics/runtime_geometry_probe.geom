#version 450
/* The geometry witness itself. Every mode emits the same three-vertex strip for
 * the primitives it keeps, so the only difference between modes is which
 * primitives survive, where their vertices go and what varying they carry. */
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(constant_id = 0) const int MODE = 0;
layout(location = 0) in vec3 color[];
layout(location = 0) out vec3 out_color;
void main()
{
    /* 0 passthrough, 1 shrink, 2 suppress, 3 rewrite the varying. The
     * per-primitive input (gl_PrimitiveIDIn) is not part of this witness yet:
     * the interface policy refuses built-ins the adapter cannot deliver, and
     * this stage's per-primitive delivery has not been established. */
    if (MODE == 2) { return; }
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

#version 450

/* ViewportIndex routing witness (DXVK262-T05, multiViewport end to end).
 *
 * Core Vulkan lets only a geometry stage select a viewport, so this stage
 * turns every input triangle into one full-viewport quad and routes it to
 * the viewport numbered by the primitive's ordinal: primitive i lands in
 * viewport i. The consumer lays the sixteen viewports out as a 4 x 4 grid of
 * tiles, so tile i must hold exactly the colour of input triangle i and
 * nothing else - a draw that ignored the index paints only tile 0 with the
 * last colour, a broadcast paints every tile with the last colour, and a
 * wrong bank permutes the tiles.
 */
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;

layout(location = 0) in vec4 in_color[];
layout(location = 0) out vec4 out_color;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    for (int i = 0; i < 4; ++i) {
        gl_ViewportIndex = gl_PrimitiveIDIn;
        gl_Position = vec4(corners[i], 0.5, 1.0);
        out_color = in_color[0];
        EmitVertex();
    }
    EndPrimitive();
}

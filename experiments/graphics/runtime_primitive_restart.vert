#version 450
/* Diagnostic-only pre-raster half for the primitive-restart witness.
 *
 * Eight vertices in two axis-aligned quads with a gap between them, drawn as an
 * indexed triangle strip whose index list carries a restart index between the
 * quads. gl_VertexIndex is the INDEX VALUE in an indexed draw, so the colour
 * identifies which quad a vertex belongs to: a strip that is cut at the restart
 * index draws two separate quads, and one that is not threads a bridging
 * primitive across the gap, which the oracle sees as ink where the target must
 * be clear. */
layout(location = 0) in vec2 a_position;
layout(location = 0) out vec3 color;
void main()
{
    gl_Position = vec4(a_position, 0.5, 1.0);
    color = gl_VertexIndex < 4 ? vec3(0.75, 0.25, 0.25) : vec3(0.25, 0.75, 0.25);
}

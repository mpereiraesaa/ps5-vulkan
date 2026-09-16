#version 450
#extension GL_ARB_shader_draw_parameters : require

/* Indirect and indexed draw witness (DXVK262-T03).
 *
 * The square colour target is a grid of `cells` x `cells` equal cells. Every
 * draw places one triangle around the centre of the cell it selects, so a
 * CPU readback can attribute each covered cell to exactly one draw or
 * instance, and the triangle's own half extent (45% of a cell) never reaches
 * a neighbouring cell centre - with 256 cells per row a cell is one pixel and
 * the triangle covers exactly that pixel centre, which is how 65535 indirect
 * commands are told apart one by one.
 *
 *   select 0: cell = gl_DrawIDARB                      (multi-draw commands)
 *   select 1: cell = gl_InstanceIndex - BaseInstance   (instances of a draw)
 *   select 2: cell = gl_VertexIndex / 3                (indexed triangles)
 *
 * The corner of the triangle a vertex builds comes from the vertex ordinal:
 * gl_VertexIndex - gl_BaseVertexARB for the draw-parameter modes (the ordinal
 * for a non-indexed draw, the raw index for an indexed one), and the
 * effective gl_VertexIndex itself for select 2, whose indices are chosen so
 * that index + vertexOffset lands on 0..2 - a 32-bit index that were
 * truncated or sign-extended would move the triangle away from its cell.
 *
 *   encode 0: R = BaseVertex & 0xff, G = gl_InstanceIndex & 0xff,
 *             B = gl_DrawIDARB & 0xff            (the delivered built-ins)
 *   encode 1: R = cell & 0xff, G = cell >> 8, B = marker
 *             (the full 16-bit cell index, for the 65535-command grid)
 *
 * The bound vertex buffer holds zeroes; its one attribute exists because an
 * indexed draw must name a vertex binding, and it moves nothing visibly.
 */
layout(location = 0) in vec4 in_offset;
layout(location = 0) out vec4 out_color;
layout(push_constant) uniform Witness {
    uint cells;
    uint select;
    uint encode;
    uint marker;
} w;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    uint draw_id = uint(gl_DrawIDARB);
    uint instance_ordinal = uint(gl_InstanceIndex - gl_BaseInstanceARB);
    uint effective_vertex = uint(gl_VertexIndex);
    uint cell = w.select == 0u ? draw_id :
                w.select == 1u ? instance_ordinal :
                effective_vertex / 3u;
    uint corner = (w.select == 2u ? effective_vertex
                                  : uint(gl_VertexIndex - gl_BaseVertexARB)) % 3u;
    float size = 2.0 / float(w.cells);
    vec2 centre = vec2(-1.0) + (vec2(float(cell % w.cells), float(cell / w.cells)) + 0.5) * size;
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(0.0, 1.0));
    gl_Position = vec4(centre + corners[corner] * (size * 0.45) + in_offset.xy * 0.0001, 0.5, 1.0);
    uvec3 encoded = w.encode == 0u ?
        uvec3(uint(gl_BaseVertexARB) & 0xffu, uint(gl_InstanceIndex) & 0xffu, draw_id & 0xffu) :
        uvec3(cell & 0xffu, (cell >> 8u) & 0xffu, w.marker & 0xffu);
    out_color = vec4(vec3(encoded) / 255.0, 1.0);
}

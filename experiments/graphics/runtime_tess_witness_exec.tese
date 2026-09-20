#version 450
/* Domain-execution witness.
 *
 * The position and colour are IDENTICAL to runtime_tess_coord.tese, so the
 * image oracle is unchanged and this shader can be compared against it
 * directly. The only addition is a memory write, and that is the whole point:
 * a tessellation evaluation shader has no per-vertex input except
 * gl_TessCoord, so if it executes at all its exports must cover pixels - and
 * the measured image is empty with no foreign pixels at all. That leaves two
 * possibilities which no register can separate: the domain never executes, or
 * it executes and its exports are discarded before rasterisation.
 *
 * A storage-buffer write is the only observable a domain shader has that does
 * not depend on rasterisation. Each invocation bumps a counter and records its
 * own gl_TessCoord, so the buffer answers three questions at once: whether the
 * stage ran, how many vertices the tessellator produced, and whether
 * gl_TessCoord arrives with real values or as zeroes. */
layout(triangles, equal_spacing, cw) in;
layout(location = 0) out vec3 out_color;
layout(set = 0, binding = 0, std430) buffer Witness {
    uint count;
    uint vertex_count;
    uint pad[2];
    float coords[252];
} witness;
void main()
{
    uint slot = atomicAdd(witness.count, 1u);
    if (slot < 84u) {
        witness.coords[slot * 3u + 0u] = gl_TessCoord.x;
        witness.coords[slot * 3u + 1u] = gl_TessCoord.y;
        witness.coords[slot * 3u + 2u] = gl_TessCoord.z;
    }
    vec3 q = floor(gl_TessCoord * 2.0) / 2.0;
    gl_Position = vec4(gl_TessCoord.x * 1.8f - 0.9f,
                       gl_TessCoord.y * 1.8f - 0.9f, 0.0, 1.0);
    out_color = vec3(q.x, q.y, 0.5);
}

#version 450
/* The witness's own control.
 *
 * Identical to runtime_tess_coord.vert - three procedural corners, no vertex
 * data - plus a write to the SAME storage buffer at a different counter.
 *
 * It exists because "the domain wrote nothing" and "a storage-buffer write
 * from a graphics stage does not land on this driver" produce the same
 * reading, and the whole point of the witness is to distinguish things that
 * look alike. This half is the LS end of the merged program and is PROVEN to
 * run, because the control half's tessellation factors are in memory. So if
 * this counter increments and the domain's does not, invocations=0 means what
 * it says; if neither increments, the instrument is the problem and the
 * domain result is worthless. */
layout(set = 0, binding = 0, std430) buffer Witness {
    uint count;
    uint vertex_count;
    uint pad[2];
    float coords[252];
} witness;
void main()
{
    atomicAdd(witness.vertex_count, 1u);
    vec2 corner[3] = vec2[3](vec2(-0.9,-0.9), vec2(0.9,-0.9), vec2(0.0,0.9));
    gl_Position = vec4(corner[gl_VertexIndex], 0.0, 1.0);
}

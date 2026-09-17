#version 450
/* The coverage witness: two triangles cover the whole target with an affine
 * varying, and - in the distance build - the clip/cull distances are functions
 * of the same position. MODE selects which distances are non-positive, so one
 * module produces every case the oracle judges. */
layout(location = 0) out vec3 color;
#ifdef WITH_DISTANCES
layout(constant_id = 0) const int MODE = 0;
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[2];
    float gl_CullDistance[2];
};
#endif
void main()
{
    /* Extended past every target edge, so the only coverage boundary that can
     * appear is the one a distance plane draws. */
    const vec2 positions[6] = vec2[6](
        vec2(-1.2, -1.2), vec2(1.2, -1.2), vec2(-1.2, 1.2),
        vec2(1.2, -1.2), vec2(1.2, 1.2), vec2(-1.2, 1.2));
    vec2 p = positions[gl_VertexIndex % 6];
    gl_Position = vec4(p, 0.5, 1.0);
    /* The varying is affine in the position, so the oracle can predict every
     * covered pixel's colour exactly instead of comparing against a pattern. */
    color = vec3((p.x + 1.0) * 0.5, (p.y + 1.0) * 0.5, 0.5);
#ifdef WITH_DISTANCES
    /* Never a literal: a constant distance could be folded away, and the point
     * of the witness is the export path, not a compile-time decision. */
    float clip0 = 1.0 + 0.25 * p.x;
    float clip1 = 1.0 + 0.25 * p.y;
    float cull0 = 1.0 + 0.25 * p.x;
    float cull1 = 1.0 + 0.25 * p.y;
    if (MODE == 1) { clip0 = p.x; }
    if (MODE == 2) { clip0 = p.x; clip1 = p.y; }
    if (MODE == 3) { cull0 = p.x; }
    if (MODE == 4) { cull0 = -1.0 - 0.25 * p.x; cull1 = -1.0 - 0.25 * p.y; }
    if (MODE == 5) { clip0 = p.x; clip1 = p.y; }
    /* One cull index negative everywhere and another mixed: the primitive is
     * still discarded, because the rule is per half-space, not "any vertex". */
    if (MODE == 6) { cull0 = p.x; cull1 = -1.0 - 0.25 * p.y; }
    if (MODE == 7) {
        /* The quadrant clip, written through a NON-constant index: the upstream
         * family registers this variant as `*_dynamic_index`, and the compiler
         * has to keep the full-width mask for it. Both elements are written
         * exactly once per vertex (the loop permutes the index by the vertex
         * index, so the two iterations cover {0,1} for every vertex), and the
         * expected image is the quadrant's. */
        const float values[2] = float[2](p.x, p.y);
        gl_CullDistance[0] = cull0;
        gl_CullDistance[1] = cull1;
        for (int i = 0; i < 2; ++i) {
            int index = (i + gl_VertexIndex) & 1;
            gl_ClipDistance[index] = values[index];
        }
        return;
    }
    gl_ClipDistance[0] = clip0;
    gl_ClipDistance[1] = clip1;
    gl_CullDistance[0] = cull0;
    gl_CullDistance[1] = cull1;
#endif
}

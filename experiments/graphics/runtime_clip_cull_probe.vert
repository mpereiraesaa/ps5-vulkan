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
    gl_ClipDistance[0] = clip0;
    gl_ClipDistance[1] = clip1;
    gl_CullDistance[0] = cull0;
    gl_CullDistance[1] = cull1;
#endif
}

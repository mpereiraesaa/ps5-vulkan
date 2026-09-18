#version 450
/* Tessellation witness vertex half: two triangle patches side by side. Patch
 * zero occupies the left half of the frame, patch one the right; the vertex's
 * x coordinate also tells the control half which patch it belongs to. The
 * colour is the identity the evaluation stage's quantised tessCoord field is
 * checked against (the witness colour is derived from the tessCoord, so the
 * pass-through colour only bounds the input interface). */
layout(location = 0) out vec3 out_color;
void main()
{
    /* Two patches, three control points each: left triangle and right
     * triangle, both covering their half of the centred square. */
    vec2 corner[6] = vec2[6](
        vec2(-0.9, -0.9), vec2(-0.1, -0.9), vec2(-0.5,  0.9),
        vec2( 0.1, -0.9), vec2( 0.9, -0.9), vec2( 0.5,  0.9));
    int v = gl_VertexIndex;
    gl_Position = vec4(corner[v], 0.0, 1.0);
    out_color = vec3(0.25, 0.5, float(v));
}

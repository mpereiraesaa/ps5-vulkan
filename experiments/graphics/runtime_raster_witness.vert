#version 450

/* Rasterization-state witness (DXVK262-T05).
 *
 * Every vertex carries its own clip-space position and colour, so the witness
 * cases can place coplanar floors, tilted planes, planes crossing the near and
 * far clip planes with a non-unit W, and triangles whose vertices sit exactly
 * on pixel centres, without any stage computing geometry from built-ins. The
 * fragment stage writes the interpolated colour unchanged, and every colour a
 * case uses is exact in UNORM8, so a pixel is attributed to exactly one draw
 * or to the clear.
 */
layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    gl_Position = in_position;
    out_color = in_color;
}

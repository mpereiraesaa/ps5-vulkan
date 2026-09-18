#version 450
/* The zero-level control: the same control half with every tessellation
 * level at zero, so the tessellator generates nothing and the hull makes no
 * factor writes - the draw isolates the LS/HS launch itself from the factor
 * path. */
layout(vertices = 3) out;
void main()
{
    gl_TessLevelOuter[0] = 0.0;
    gl_TessLevelOuter[1] = 0.0;
    gl_TessLevelOuter[2] = 0.0;
    gl_TessLevelInner[0] = 0.0;
}

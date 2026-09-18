#version 450
/* TessCoord control's control half: publishes the tessellation levels and
 * NOTHING else - no input reads, no per-vertex output writes, so the hull's
 * off-chip path stays out of the picture and the draw isolates the
 * tessellator + the evaluation half's own launch. */
layout(vertices = 3) out;
void main()
{
    gl_TessLevelOuter[0] = 2.0;
    gl_TessLevelOuter[1] = 2.0;
    gl_TessLevelOuter[2] = 2.0;
    gl_TessLevelInner[0] = 1.0;
}

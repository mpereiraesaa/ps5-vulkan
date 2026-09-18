#version 450
/* The TessCoord control's control half with HIGH tessellation levels.
 *
 * Identical to runtime_tess_coord.tesc - publishes the levels and nothing
 * else, no input reads, no per-vertex output writes - except that the levels
 * are 16 instead of 2 and 1.
 *
 * It exists to test a QUANTITATIVE hypothesis rather than another boolean
 * register. Every patch drawn in this task so far has been outer 2, 2, 2 with
 * inner 1, which tessellates to four triangles. This driver programs
 * VGT_TESS_DISTRIBUTION with the pinned emitter's accumulators, whose
 * triangle accumulator is ELEVEN, and the geometry engine batches
 * tessellation work before dispatching it. Four is less than eleven. If the
 * domain never launches because the work never reaches a threshold rather
 * than because something is misconfigured, no register experiment can show
 * it and a bigger patch shows it immediately: outer 16 with inner 16
 * tessellates to hundreds of triangles. */
layout(vertices = 3) out;
void main()
{
    gl_TessLevelOuter[0] = 16.0;
    gl_TessLevelOuter[1] = 16.0;
    gl_TessLevelOuter[2] = 16.0;
    gl_TessLevelInner[0] = 16.0;
}

#version 440
/* DXVK262-T06 sample-rate witness vertex module: one oversized triangle whose
 * coverage is the whole target, so every pixel of the readback is produced by
 * the per-sample shaded draw and no part of the surface keeps the clear value.
 * It declares no output at all - the fragment module reads only gl_SampleID -
 * which keeps the readback a statement about iteration count alone. */
void main (void)
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1.0);
}

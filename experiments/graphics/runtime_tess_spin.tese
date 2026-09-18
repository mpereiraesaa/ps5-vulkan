#version 450
/* Domain-execution witness that needs NO DESCRIPTOR.
 *
 * The storage-buffer witness cannot be validated on this driver: its control
 * draw is refused in the compile of a vertex shader carrying a storage
 * buffer, so "the domain wrote nothing" and "graphics-stage storage writes do
 * not land here" stay indistinguishable. This shader removes the descriptor
 * from the question entirely by making the evaluation half COST TIME instead
 * of writing memory.
 *
 * The loop is long but FINITE and its result feeds the position, so it cannot
 * be optimised away. If the domain executes, the draw takes long enough to
 * exceed the harness's 300 ms bounded fence wait and the run reports
 * PS5VK_TESS_STALL - a log line the harness already emits. If it does not
 * execute, the draw retires in its usual 0.168 s with no stall line. One bit
 * of information, read off a mechanism that already exists, with no
 * descriptor, no buffer and no readback in the path.
 *
 * Deliberately finite: an infinite loop would wedge the GPU, which is exactly
 * what cost a previous agent an owner-side reboot. Two hundred million
 * dependent iterations is a few hundred milliseconds - past the fence, far
 * short of the watchdog that fired at about a second and a half. */
layout(triangles, equal_spacing, cw) in;
layout(location = 0) out vec3 out_color;
void main()
{
    float spin = gl_TessCoord.x;
    for (int i = 0; i < 200000000; ++i)
        spin = spin * 1.0000001 + 1e-9;
    vec3 q = floor(gl_TessCoord * 2.0) / 2.0;
    gl_Position = vec4(gl_TessCoord.x * 1.8f - 0.9f + spin * 1e-30f,
                       gl_TessCoord.y * 1.8f - 0.9f, 0.0, 1.0);
    out_color = vec3(q.x, q.y, 0.5);
}

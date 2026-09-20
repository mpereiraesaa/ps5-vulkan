#version 450
/* THE POSITIVE CONTROL for the spin witness.
 *
 * The descriptor-free domain witness reads "no stall line" as "the stage did
 * not execute". That inference has never been checked in the other
 * direction: nobody has shown that a tessellation stage which DOES execute
 * produces a stall line on this hardware. Without that, "no stall" could
 * equally mean the loop was optimised away, or that a few hundred million
 * iterations are simply not slow enough here.
 *
 * The control half is the ideal control because it is PROVEN to execute -
 * its tessellation factors are in the ring, read back at the address its own
 * SRD names, in every run for two different domains. So this is the same
 * spin, in the stage that certainly runs.
 *
 * If this stalls, the instrument works and every domain result taken with it
 * stands. If it does not, the instrument never worked, and the central
 * finding of this session - that the evaluation half does not execute - is
 * unsupported and I withdraw it.
 *
 * The result feeds the tessellation levels so it cannot be optimised away,
 * and it is scaled to leave the levels exactly 2.0 and 1.0 so the factors
 * stay byte-identical to the control fixture and remain independently
 * checkable in the ring. */
layout(vertices = 3) out;
void main()
{
    float spin = float(gl_InvocationID);
    for (int i = 0; i < 200000000; ++i)
        spin = spin * 1.0000001 + 1e-9;
    gl_TessLevelOuter[0] = 2.0 + spin * 1e-30;
    gl_TessLevelOuter[1] = 2.0;
    gl_TessLevelOuter[2] = 2.0;
    gl_TessLevelInner[0] = 1.0;
}

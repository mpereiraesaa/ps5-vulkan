#version 450

/* The mixed-MRT shape the upstream independentBlend oracle builds: attachment
 * zero is an integer target (R8G8B8A8_UINT) and attachment one a normalized
 * one (R8G8B8A8_UNORM), so the fragment stage exports a uvec4 at Location 0 and
 * a vec4 at Location 1. Used to measure what the pinned compiler publishes for
 * an integer export and to drive the two-target write-mask leaf afterwards. */
layout(location = 0) out uvec4 mrt0_uint;
layout(location = 1) out vec4 mrt1_float;

void main()
{
    mrt0_uint = uvec4(51u, 102u, 153u, 255u);
    mrt1_float = vec4(0.80, 0.40, 0.20, 0.50);
}

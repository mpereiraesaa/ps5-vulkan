#version 450

/* The shape the upstream independentBlend oracle builds when the FIRST colour
 * target's write mask is zero: the fragment stage exports Location 1 alone.
 * The pinned compiler publishes that as SPI_SHADER_COL_FORMAT=0x9 with
 * CB_SHADER_MASK=0xf0 - its format nibbles follow the module's outputs in
 * declaration order while the mask names the attachment each export targets -
 * so this module is what pins the driver's normalisation of that pair. */
layout(location = 1) out vec4 mrt1_only;

void main()
{
    mrt1_only = vec4(0.25, 0.5, 0.75, 1.0);
}

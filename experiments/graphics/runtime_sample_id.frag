#version 440
/* DXVK262-T06 sample-rate witness module: the red channel carries the sample
 * index the pixel invocation was launched for, so a per-sample shaded draw
 * leaves one distinct value per sample plane and a once-per-pixel draw leaves
 * exactly one value in the whole surface. Nothing else varies between samples,
 * so the readback cannot be satisfied by interpolation or coverage. */
layout(location = 0) out vec4 fragColor;

void main (void)
{
    fragColor = vec4(float(gl_SampleID) / 255.0, 0.0, 0.0, 1.0);
}

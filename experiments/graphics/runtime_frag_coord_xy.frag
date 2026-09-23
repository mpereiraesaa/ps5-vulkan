#version 440
/* DXVK262-T06 sample-rate fragment-coordinate witness.
 *
 * The pinned min_sample_shading leaves colour each pixel with
 * fract(gl_FragCoord.xy) and then require as many DISTINCT colours across the
 * per-sample images as round(minSampleShading * samples). That oracle can only
 * be satisfied if the fragment coordinate is the SAMPLE's, so this module is
 * the measurement that isolates it: drawn with per-sample shading into a
 * multisampled target, a source census with one distinct value means every
 * sample received the same coordinate (the pixel centre), and one value per
 * sample means the hardware really interpolates per sample. */
layout(location = 0) out vec4 fragColor;

void main (void)
{
    fragColor = vec4(fract(gl_FragCoord.xy), 0.0, 1.0);
}

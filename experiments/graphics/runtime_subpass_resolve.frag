#version 440
/* DXVK262-T06 shape walk, resolve variant: the fragment stage reads every
 * sample of the multisampled input attachment and writes their average to the
 * subpass's own single-sample target. The four reads are written out rather
 * than looped, because the profile's fragment interface has only ever been
 * measured on straight-line subpassLoad reads. */
layout(location = 0) out vec4 fs_out_color;

layout(set = 0, binding = 0, input_attachment_index = 0) uniform subpassInputMS imageMS;

void main (void)
{
    vec4 sum = subpassLoad(imageMS, 0) + subpassLoad(imageMS, 1) +
               subpassLoad(imageMS, 2) + subpassLoad(imageMS, 3);
    fs_out_color = sum * 0.25;
}

#version 440
/* DXVK262-T06 shape walk, resolve oracle: subpass 0 of the oracle's own pass
 * with a per-sample output whose AVERAGE no sample can hold. Sample k writes
 * R = 4(k+1)/255, so the samples hold 4, 8, 12 and 16 and their average is 10 -
 * a value none of them has. The plain 0,1,2,3 pattern cannot make that
 * distinction: its average (1.5, rounded to 1 or 2) IS a sample value, so a
 * target that received one sample's plane and a target that received the
 * average look the same. Starting at 4 also keeps every sample word clear of
 * the 0..3 patterns earlier phases of this payload wrote into the memory the
 * resolve target's allocation reuses, so a stale word cannot be read as a
 * sample. This module is only used to judge the driver's resolve draw; nothing
 * else varies between samples. */
layout(location = 0) out vec4 output_color;

void main()
{
    output_color = vec4(float(gl_SampleID + 1) * 4.0 / 255.0, 0.0, 0.0, 1.0);
}

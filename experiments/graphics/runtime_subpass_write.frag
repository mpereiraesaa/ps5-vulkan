#version 450
/* DXVK262-T06 shape walk, subpass 0: the multisampled colour target is drawn
 * into with a stage that has no input attachment, so the walk separates "a
 * multisampled pipeline runs at all" from "the per-sample fetch pipeline
 * exists". */
layout(location = 0) out vec4 output_color;

void main()
{
    output_color = vec4(0.25, 0.5, 0.75, 1.0);
}

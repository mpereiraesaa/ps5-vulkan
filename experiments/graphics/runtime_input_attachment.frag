// SPDX-License-Identifier: GPL-3.0-or-later
// An input attachment is resource-only image data: subpassLoad() reads the
// attachment's own pixels through the eight DWORD image record, never through
// a sampler. The fixture keeps a combined sampler in the same set and a second
// input attachment in the next set, so the layout PSBC receives has to place
// all three records at their canonical offsets and give the two input
// attachments the 32-byte stride while the combined pair keeps its 48 bytes.
#version 450
layout(set=0,binding=1) uniform sampler2D sampled_source;
layout(input_attachment_index=0,set=0,binding=3) uniform subpassInput attachment_color;
layout(input_attachment_index=0,set=1,binding=5) uniform subpassInput blend_target;
layout(location=0) out vec4 color;
void main()
{
    color=subpassLoad(attachment_color)*textureLod(sampled_source,vec2(0.5),0.0)
        +subpassLoad(blend_target);
}

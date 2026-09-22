#version 440
/* DXVK262-T06 shape walk, constant-index variant: the same per-sample fetch as
 * runtime_subpass_fetch.frag with the sample index BAKED IN rather than read
 * from a uniform block. It exists to tell two failures apart when the uniform
 * one returns the wrong plane: whether the index never reached the shader, or
 * whether the read itself ignores it. */
layout(location = 0) out vec4 fs_out_color;

layout(set = 0, binding = 0, input_attachment_index = 0) uniform subpassInputMS imageMS;

void main (void)
{
    fs_out_color = subpassLoad(imageMS, 3);
}

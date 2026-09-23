#version 440
/* DXVK262-T06 shape walk, subpass 1: the pinned multisample oracle's per-sample
 * fetch stage (external/vulkancts/modules/vulkan/pipeline/
 * vktPipelineMultisampleBaseResolveAndPerSampleFetch.cpp, initPrograms), with
 * the sample index still coming from the uniform block as upstream declares it.
 * It reads sample sampleNdx of the multisampled colour input attachment and
 * writes it to this subpass's own single-sample colour target, which is the
 * shape whose per-sample read has never been executed on this path. */
layout(location = 0) out vec4 fs_out_color;

layout(set = 0, binding = 0, input_attachment_index = 0) uniform subpassInputMS imageMS;

layout(set = 0, binding = 1, std140) uniform SampleBlock {
    int sampleNdx;
};

void main (void)
{
    fs_out_color = subpassLoad(imageMS, sampleNdx);
}

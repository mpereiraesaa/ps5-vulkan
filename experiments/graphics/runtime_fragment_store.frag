#version 450

layout(set = 0, binding = 0, std430) buffer FragmentWitness {
    uint words[];
} witness;

layout(location = 0) out vec4 out_color;

void main()
{
    atomicAdd(witness.words[0], 1u);
    out_color = vec4(0.25, 0.5, 0.75, 1.0);
}

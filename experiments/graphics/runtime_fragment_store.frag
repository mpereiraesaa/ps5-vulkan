#version 450

#ifndef CONTROL
layout(set = 0, binding = 0, std430) buffer FragmentWitness {
    uint words[];
} witness;
#endif

layout(location = 0) out vec4 out_color;

void main()
{
#ifndef CONTROL
    atomicAdd(witness.words[0], 1u);
#endif
    out_color = vec4(0.25, 0.5, 0.75, 1.0);
}

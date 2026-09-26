#version 450
/* Transform feedback witness (DXVK262-T14): every vertex carries values
 * derived only from its index, so a captured record names the vertex that
 * produced it and the oracle can check both content and order. */
layout(location = 0) out vec4 value;
void main()
{
    const uint k = uint(gl_VertexIndex);
    value = vec4(float(k), float(2u * k + 1u), float(k & 255u), 7.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    gl_PointSize = 1.0;
}

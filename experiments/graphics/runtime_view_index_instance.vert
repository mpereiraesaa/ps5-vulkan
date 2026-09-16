#version 450
#extension GL_EXT_multiview : require

/* Private maxMultiviewInstanceIndex witness (T02-D1c-1). The six-view witness
 * plus the other multiview built-in: the instance index has to arrive EXACTLY as
 * 0x07ffffff. That value is 2^27-1, which is NOT representable in a 32-bit
 * float - it rounds to 2^27 - so the shader never converts it: the test is a pure
 * integer bit test, and the result is a FIXED byte either way. When the instance
 * index is exact the per-view colour and depth of the six-view witness are kept,
 * so the view half of the proof is unchanged; when it is not, the fragment is a
 * colour no view can produce and the oracle counts it as an instance failure
 * rather than as a foreign view. */
layout(location = 0) out vec3 color;

void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    const int instance = int(gl_InstanceIndex);
    const float shade = float(gl_ViewIndex);
    /* 0x07ffffff exactly: those low 27 bits set and nothing above them. */
    const bool exact = (instance & 0x07ffffff) == 0x07ffffff && (instance >> 27) == 0;
    gl_Position = vec4(positions[gl_VertexIndex], (shade + 1.0) / 16.0, 1.0);
    color = exact ? vec3((shade + 1.0) / 16.0, shade / 8.0, 0.5) : vec3(1.0, 0.0, 1.0);
}

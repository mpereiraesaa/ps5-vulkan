#version 450
/* Diagnostic-only pre-raster half for the geometry per-primitive id.
 *
 * Every applicable upstream geometry leaf declares gl_PrimitiveIDIn, so a stage
 * that can report which primitive it is processing is the gateway to the
 * feature's conformance leaves. Each invocation emits one marker at a place and
 * with a colour derived from the id the hardware gave it, so one image says both
 * that the id arrived and that it is the primitive's: a stage that read a stale
 * or constant id puts every marker at the same place with the same colour.
 */
layout(triangles) in;
layout(triangle_strip, max_vertices = 9) out;
layout(constant_id = 0) const int MODE = 18;
layout(location = 0) in vec3 in_v[];
layout(location = 0) out vec3 out_v;
void main()
{
    float column = -0.6 + 0.6 * float(gl_PrimitiveIDIn);
    const vec2 offsets[4] = vec2[4](vec2(-0.02, -0.2), vec2(0.02, -0.2),
                                    vec2(-0.02, 0.2), vec2(0.02, 0.2));
    for (int i = 0; i < 4; ++i) {
        gl_Position = vec4(column + offsets[i].x, offsets[i].y, 0.5, 1.0);
        out_v = vec3(float(gl_PrimitiveIDIn), 0.5, 0.25);
        EmitVertex();
    }
    EndPrimitive();
}

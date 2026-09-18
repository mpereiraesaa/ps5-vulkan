#version 450
/* Diagnostic-only pre-raster half for the geometry invocations minimum.
 *
 * maxGeometryShaderInvocations is a mandatory minimum of the geometry feature
 * (32), and a stage can only use more than one invocation if it can tell which
 * one it is. Each invocation emits its own marker on its own column and colours
 * it by its invocation id, so one image reports both that all 32 invocations ran
 * and that each saw the id it should: a stage that ran once covers one column, a
 * stage that misdelivered the id colours the columns wrongly. `invocations` is a
 * module-level declaration, so this too is a module of its own.
 */
layout(invocations = 32, triangles) in;
layout(triangle_strip, max_vertices = 9) out;
layout(constant_id = 0) const int MODE = 16;
layout(location = 0) in vec3 in_v[];
layout(location = 0) out vec3 out_v;
void main()
{
    float column = -0.72 + float(gl_InvocationID) * 0.045;
    const vec2 offsets[4] = vec2[4](vec2(-0.02, -0.2), vec2(0.02, -0.2),
                                    vec2(-0.02, 0.2), vec2(0.02, 0.2));
    for (int i = 0; i < 4; ++i) {
        gl_Position = vec4(column + offsets[i].x, offsets[i].y, 0.5, 1.0);
        out_v = vec3(float(gl_InvocationID) / 31.0, 0.5, 0.25);
        EmitVertex();
    }
    EndPrimitive();
}

#version 450
/* Tessellation control stage: pass every control point through unchanged and
 * publish fixed tessellation levels, so the evaluated patch is a deterministic
 * function of the levels alone. The output vertex count is the patch control
 * point count the pipeline state must agree with. */
layout(vertices = 3) out;
layout(location = 0) in vec3 color[];
layout(location = 0) out vec3 out_color[];
/* One per-patch value: it must reach the evaluation stage through the patch
 * interface, not through the per-vertex one. */
layout(location = 1) patch out float patch_tag;
void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    out_color[gl_InvocationID] = color[gl_InvocationID];
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = 3.0;
        gl_TessLevelOuter[1] = 3.0;
        gl_TessLevelOuter[2] = 3.0;
        gl_TessLevelInner[0] = 2.0;
        patch_tag = 0.25;
    }
}

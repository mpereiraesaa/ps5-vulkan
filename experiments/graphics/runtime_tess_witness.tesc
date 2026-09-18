#version 450
/* Tessellation witness control half: pass every control point through and
 * publish the levels per patch - the left patch (whose first vertex is left of
 * centre) tessellates at level three, the right at level one. The quantised
 * tessCoord field the evaluation stage paints is an exact function of these
 * levels, so a stage that lost a level or mixed the patches cannot pass. */
layout(vertices = 3) out;
layout(location = 0) in vec3 in_color[];
layout(location = 0) out vec3 out_color[];
void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    out_color[gl_InvocationID] = in_color[gl_InvocationID];
    if (gl_InvocationID == 0) {
        bool left = gl_in[0].gl_Position.x < 0.0;
        float level = left ? 3.0 : 1.0;
        gl_TessLevelOuter[0] = level;
        gl_TessLevelOuter[1] = level;
        gl_TessLevelOuter[2] = level;
        gl_TessLevelInner[0] = left ? 2.0 : 0.0;
        gl_TessLevelInner[1] = left ? 2.0 : 0.0;
    }
}

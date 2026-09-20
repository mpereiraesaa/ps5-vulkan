#version 450
/* Tessellation witness evaluation half: triangles with equal spacing and the
 * clockwise order, painting the QUANTISED tessellation coordinate - the
 * witness colour is an exact function of gl_TessCoord and the patch's level,
 * so the image names the sub-triangle it came from and a stage that dropped
 * the tessellator (or a level) paints a different, computable image. */
layout(triangles, equal_spacing, cw) in;
layout(location = 0) in vec3 in_color[];
layout(location = 0) out vec3 out_color;
void main()
{
    bool left = gl_in[0].gl_Position.x < 0.0;
    float level = left ? 3.0 : 1.0;
    vec3 q = floor(gl_TessCoord * level) / level;
    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position +
                  gl_TessCoord.y * gl_in[1].gl_Position +
                  gl_TessCoord.z * gl_in[2].gl_Position;
    out_color = vec3(q.x, q.y, left ? 0.125 : 0.625);
}

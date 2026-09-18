#version 450
/* TessCoord control's evaluation half: the position is a pure function of
 * gl_TessCoord - NO off-chip input reads - and the colour names the quantised
 * coordinate, so the image proves the tessellator ran and the domain half's
 * own launch state is sound. */
layout(triangles, equal_spacing, cw) in;
layout(location = 0) out vec3 out_color;
void main()
{
    vec3 q = floor(gl_TessCoord * 2.0) / 2.0;
    gl_Position = vec4(gl_TessCoord.x * 1.8f - 0.9f,
                       gl_TessCoord.y * 1.8f - 0.9f, 0.0, 1.0);
    out_color = vec3(q.x, q.y, 0.5);
}

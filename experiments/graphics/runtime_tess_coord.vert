#version 450
/* TessCoord control's vertex half: three procedural corner positions, no
 * vertex data at all. The control half does not read them; they only feed
 * the patch assembler's control point count. */
void main()
{
    vec2 corner[3] = vec2[3](vec2(-0.9,-0.9), vec2(0.9,-0.9), vec2(0.0,0.9));
    gl_Position = vec4(corner[gl_VertexIndex], 0.0, 1.0);
}

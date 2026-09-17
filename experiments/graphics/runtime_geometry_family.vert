#version 450
/* Diagnostic-only pre-raster half for the input-family cases.
 *
 * A device that advertises geometryShader is expected to feed a stage points and
 * lines as well as triangles, and the witness has to say whether the stage read
 * the items it was given. Vertex index i therefore carries a position and a
 * colour that identify it - the table below is the same table
 * src/geometry_witness.c mirrors in ps5vk_geometry_witness_family_vertex - so a
 * marker a geometry stage places from what it read NAMES the item that read
 * returned. The point case draws the first four vertices as a point list, the
 * line case all six as three segments of a line list; the positions are far
 * enough apart that the markers cannot touch, and far enough from the target's
 * edges that a clamped marker is still a marker.
 */
layout(location = 0) out vec3 color;
void main()
{
    const vec2 positions[6] = vec2[6](
        vec2(-0.6, -0.5), vec2(-0.2, -0.5), vec2(0.2, 0.3),
        vec2(0.7, 0.3), vec2(-0.7, 0.6), vec2(0.1, 0.6));
    int index = int(gl_VertexIndex) % 6;
    vec2 p = positions[index];
    gl_Position = vec4(p, 0.5, 1.0);
    /* i/5 is 0, 0.2, 0.4, 0.6, 0.8, 1.0: exact in binary once quantised to
     * unorm, and distinct per item, so a marker's colour identifies its input. */
    color = vec3(float(index) / 5.0, 0.5, 0.25);
}

#version 450
/* Diagnostic-only pre-raster half for the geometry envelope.
 *
 * maxGeometryOutputVertices is a mandatory minimum of the geometry feature (256),
 * so the stage must really be able to emit that many vertices. `max_vertices` is
 * a module-level declaration, which is why this is its own module rather than
 * another mode of the witness geometry stage.
 *
 * The emission is a single 256-vertex triangle-strip ribbon: two vertices per
 * column alternating between the band's two edges, so the strip tiles the whole
 * band and the image proves that every one of the 256 vertices was emitted - a
 * stage that stopped early would cover a shorter band and fail the oracle.
 */
layout(triangles) in;
layout(triangle_strip, max_vertices = 256) out;
layout(constant_id = 0) const int MODE = 15;
layout(location = 0) in vec3 in_v[];
layout(location = 0) out vec3 out_v;
void main()
{
    for (int i = 0; i < 256; ++i) {
        float column = float(i / 2);
        float x = -0.75 + column * (1.5 / 127.0);
        float y = (i % 2 == 0) ? -0.25 : 0.25;
        gl_Position = vec4(x, y, 0.5, 1.0);
        out_v = vec3(0.25, 0.5, 0.75);
        EmitVertex();
    }
    EndPrimitive();
}

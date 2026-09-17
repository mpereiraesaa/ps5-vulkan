#version 450
/* Diagnostic-only pre-raster half for the geometry descriptor case.
 *
 * The pinned conformance module binds a uniform buffer and a sampled image to
 * the GEOMETRY stage and lets the geometry shader read them - that is what
 * dEQP-VK.geometry.basic.output_vary_by_uniform and ..._by_texture do. This
 * module is the same shape with a uniform buffer: the geometry half reads
 * location zero of the buffer and multiplies the varying by it, so a run whose
 * descriptor did not reach the merged pre-raster program draws the wrong
 * colour rather than failing to run.
 */
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(constant_id = 0) const int MODE = 21;
layout(set = 0, binding = 0) uniform Control {
    vec4 tint[4];
} control;
layout(location = 0) in vec3 color[];
layout(location = 0) out vec3 out_color;
void main()
{
    vec3 factor = control.tint[0].xyz;
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        out_color = color[i] * factor;
        EmitVertex();
    }
    EndPrimitive();
}

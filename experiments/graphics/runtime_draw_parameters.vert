#version 450
#extension GL_ARB_shader_draw_parameters : require

/* Shader draw parameters witness.
 *
 * The three built-ins are uniform for a whole draw, so the shader encodes the
 * raw values into the colour and a CPU readback can verify exactly what the GPU
 * delivered:
 *
 *   R = low byte of gl_BaseVertexARB   (two's complement when negative)
 *   G = low byte of gl_BaseInstanceARB
 *   B = low byte of gl_DrawIDARB       (zero for the draws this profile supports)
 *   A = 255
 *
 * Positions are generated from gl_VertexIndex, which must equal
 * BaseVertex + the fetched index (or firstVertex + the vertex ordinal when the
 * draw is not indexed), so the geometry itself is a second, independent check
 * of the BaseVertex contract: the witness draws index values chosen so that the
 * resulting vertex indices stay inside this three-entry table.
 */
layout(location = 0) out vec4 out_color;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    const vec2 positions[3] = vec2[3](vec2(-0.8, -0.8), vec2(0.8, -0.8), vec2(0.0, 0.8));
    uint base_vertex = uint(gl_BaseVertexARB);
    uint base_instance = uint(gl_BaseInstanceARB);
    uint draw_index = uint(gl_DrawIDARB);
    gl_Position = vec4(positions[gl_VertexIndex % 3], 0.5, 1.0);
    out_color = vec4(float(base_vertex & 0xffu) / 255.0,
                     float(base_instance & 0xffu) / 255.0,
                     float(draw_index & 0xffu) / 255.0,
                     1.0);
}

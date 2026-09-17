#version 450
/* Diagnostic-only pre-raster half for the ES/GS readback.
 *
 * The witness vertex stage gives the six vertices of its two triangles the
 * values +/-1.2, so a value the geometry half read can only say "positive" or
 * "negative" and a read that landed on another item looks like a read that
 * landed on the right one. This stage instead gives vertex i a position whose x
 * is unique per index and exactly representable - -0.6875 + 0.0625 * i - so the
 * bytes the geometry half reads back NAME the item they came from: the value
 * identifies the vertex, and the vertex identifies the ES item the ES stored it
 * in. It is used only by the readback cases. */
layout(location = 0) out vec3 color;
void main()
{
    float index = float(gl_VertexIndex);
    gl_Position = vec4(-0.6875 + 0.0625 * index, -0.3, 0.5, 1.0);
    /* The readback reads positions only, so this output is never stored in the
     * ring (the lowering drops outputs no stage reads) - it exists so the module
     * keeps the witness pipeline's interface. */
    color = vec3(0.0);
}

#version 450
/* T09 depth/stencil witness: the coverage of stencil bit k, as geometry.
 *   k < 6   the cells of a 2^k-pixel checkerboard whose x and y cell
 *           indices differ in parity: exactly the pixels where bit k of
 *           (x ^ y) is set;
 *   k = 6,7 the rows where bit k - 2 of y is set.
 * Drawn with the stencil op INVERT under write mask 1 << k, the eight draws
 * turn the cleared value c into c ^ ((x ^ y) & 63) ^ (((y >> 4) & 3) << 6),
 * a pattern every coordinate bit of the 64x64 target changes. */
layout(push_constant) uniform Bit { uint k; } bit;
void main()
{
    const vec2 corner[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                   vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    uint cell = uint(gl_VertexIndex) / 6u;
    vec2 origin, size;
    if (bit.k < 6u) {
        uint side = 1u << bit.k;
        uint cells = 64u >> bit.k;
        uint half_row = cells / 2u;
        uint row = cell / half_row;
        uint column = 2u * (cell % half_row) + ((row + 1u) & 1u);
        origin = vec2(float(column * side), float(row * side));
        size = vec2(float(side));
    } else {
        uint height = 1u << (bit.k - 2u);
        origin = vec2(0.0, float((2u * cell + 1u) * height));
        size = vec2(64.0, float(height));
    }
    vec2 pixel = origin + corner[gl_VertexIndex % 6] * size;
    gl_Position = vec4(pixel / 32.0 - 1.0, 0.5, 1.0);
}

#version 450
// DXVK262-T10 render witness: quad 0 writes an exact gradient of the pixel
// coordinates (R = 4x, G = 4y, B = 64, A = 255 as UNORM8), the markers a
// solid green or blue.
layout(location = 0) flat in uint quad;
layout(location = 0) out vec4 color;
void main()
{
    if (quad == 0u)
        color = vec4(floor(gl_FragCoord.x) * (4.0 / 255.0),
                     floor(gl_FragCoord.y) * (4.0 / 255.0), 64.0 / 255.0, 1.0);
    else if (quad == 1u)
        color = vec4(0.0, 1.0, 0.0, 1.0);
    else
        color = vec4(0.0, 0.0, 1.0, 1.0);
}

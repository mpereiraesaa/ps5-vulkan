#version 450
/* Diagnostic-only pre-raster half for the geometry component envelope.
 *
 * It declares and READS all 64 input components its predecessor exports (sixteen
 * vec4s) and folds them into the colour it draws, so the image asserts that the
 * whole declaration arrived through the ES->GS handoff rather than only that it
 * compiled. It also WRITES more than 64 output components of its own (sixteen
 * vec4s plus the three-component varying the fragment stage reads, the position
 * counted with them), which is the output side of the same minimum.
 */
layout(triangles) in;
layout(triangle_strip, max_vertices = 9) out;
layout(constant_id = 0) const int MODE = 17;
layout(location = 0) in vec4 g0[];
layout(location = 1) in vec4 g1[];
layout(location = 2) in vec4 g2[];
layout(location = 3) in vec4 g3[];
layout(location = 4) in vec4 g4[];
layout(location = 5) in vec4 g5[];
layout(location = 6) in vec4 g6[];
layout(location = 7) in vec4 g7[];
layout(location = 8) in vec4 g8[];
layout(location = 9) in vec4 g9[];
layout(location = 10) in vec4 g10[];
layout(location = 11) in vec4 g11[];
layout(location = 12) in vec4 g12[];
layout(location = 13) in vec4 g13[];
layout(location = 14) in vec4 g14[];
layout(location = 15) in vec4 g15[];
layout(location = 0) out vec3 out_v;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;
layout(location = 8) out vec4 o8;
layout(location = 9) out vec4 o9;
layout(location = 10) out vec4 o10;
layout(location = 11) out vec4 o11;
layout(location = 12) out vec4 o12;
layout(location = 13) out vec4 o13;
layout(location = 14) out vec4 o14;
layout(location = 15) out vec4 o15;
layout(location = 16) out vec4 o16;
void main()
{
    /* Every one of the 64 input components of the primitive's first vertex. The
     * values the pre-raster stage writes sum to 576, so the colour below is an
     * exact function of all of them at once: a component that did not arrive, or
     * arrived from another vertex, changes it. */
    vec4 acc4 = (g0[0] + g1[0] + g2[0] + g3[0]) +
                (g4[0] + g5[0] + g6[0] + g7[0]) +
                (g8[0] + g9[0] + g10[0] + g11[0]) +
                (g12[0] + g13[0] + g14[0] + g15[0]);
    float acc = acc4.x + acc4.y + acc4.z + acc4.w;
    const vec2 corners[4] = vec2[4](vec2(-0.5, -0.5), vec2(0.5, -0.5),
                                    vec2(-0.5, 0.5), vec2(0.5, 0.5));
    for (int i = 0; i < 4; ++i) {
        gl_Position = vec4(corners[i], 0.5, 1.0);
        out_v = vec3(acc / 4096.0, 0.5, 0.25);
        o1 = o2 = o3 = o4 = o5 = o6 = o7 = o8 = vec4(1.0);
        o9 = o10 = o11 = o12 = o13 = o14 = o15 = o16 = vec4(2.0);
        EmitVertex();
    }
    EndPrimitive();
}

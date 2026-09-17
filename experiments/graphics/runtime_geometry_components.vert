#version 450
/* Diagnostic-only pre-raster half for the geometry component envelope.
 *
 * maxGeometryInputComponents and maxGeometryOutputComponents are mandatory
 * minima of the geometry feature (64 each), so a stage must be able to declare
 * and carry that many. This stage exports sixteen vec4 varyings - 64 components
 * - whose values are distinct per location and per component, so a geometry half
 * that reads them can fold them into something the oracle can predict exactly.
 */
layout(location = 0) out vec4 v0;
layout(location = 1) out vec4 v1;
layout(location = 2) out vec4 v2;
layout(location = 3) out vec4 v3;
layout(location = 4) out vec4 v4;
layout(location = 5) out vec4 v5;
layout(location = 6) out vec4 v6;
layout(location = 7) out vec4 v7;
layout(location = 8) out vec4 v8;
layout(location = 9) out vec4 v9;
layout(location = 10) out vec4 v10;
layout(location = 11) out vec4 v11;
layout(location = 12) out vec4 v12;
layout(location = 13) out vec4 v13;
layout(location = 14) out vec4 v14;
layout(location = 15) out vec4 v15;
void main()
{
    const vec2 positions[6] = vec2[6](
        vec2(-1.2, -1.2), vec2(1.2, -1.2), vec2(-1.2, 1.2),
        vec2(1.2, -1.2), vec2(1.2, 1.2), vec2(-1.2, 1.2));
    vec2 p = positions[gl_VertexIndex % 6];
    gl_Position = vec4(p, 0.5, 1.0);
    v0  = vec4( 0.0,  1.0,  2.0,  3.0);
    v1  = vec4( 1.0,  2.0,  3.0,  4.0);
    v2  = vec4( 2.0,  3.0,  4.0,  5.0);
    v3  = vec4( 3.0,  4.0,  5.0,  6.0);
    v4  = vec4( 4.0,  5.0,  6.0,  7.0);
    v5  = vec4( 5.0,  6.0,  7.0,  8.0);
    v6  = vec4( 6.0,  7.0,  8.0,  9.0);
    v7  = vec4( 7.0,  8.0,  9.0, 10.0);
    v8  = vec4( 8.0,  9.0, 10.0, 11.0);
    v9  = vec4( 9.0, 10.0, 11.0, 12.0);
    v10 = vec4(10.0, 11.0, 12.0, 13.0);
    v11 = vec4(11.0, 12.0, 13.0, 14.0);
    v12 = vec4(12.0, 13.0, 14.0, 15.0);
    v13 = vec4(13.0, 14.0, 15.0, 16.0);
    v14 = vec4(14.0, 15.0, 16.0, 17.0);
    v15 = vec4(15.0, 16.0, 17.0, 18.0);
}

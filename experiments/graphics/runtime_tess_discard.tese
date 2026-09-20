// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
#if MATRIX_DOMAIN==0
layout(triangles) in;
#elif MATRIX_DOMAIN==1
layout(quads) in;
#else
layout(isolines) in;
#endif
#if MATRIX_SPACING==0
layout(equal_spacing) in;
#elif MATRIX_SPACING==1
layout(fractional_even_spacing) in;
#else
layout(fractional_odd_spacing) in;
#endif
layout(cw,point_mode) in;
out gl_PerVertex { vec4 gl_Position; };
layout(location=0) out vec3 out_color;
void main() {
    // Each patch has its own pixel. Collapsing generated points intentionally
    // tests visible survival only, not number of evaluation invocations.
    int patch_id=gl_PrimitiveID;
    vec2 pixel=vec2(6.5)+12.0*vec2(patch_id%5,patch_id/5);
    gl_Position=vec4(pixel/32.0-1.0,0,1);
    out_color=vec3(float(patch_id+1)/32.0,1.0,0.5);
}

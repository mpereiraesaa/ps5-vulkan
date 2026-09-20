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
    vec2 p=gl_TessCoord.xy;
    gl_Position=vec4((4.5+54.0*p)/32.0-1.0,0,1);
    out_color=vec3(p,0.5);
}

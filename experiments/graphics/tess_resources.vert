// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(location=0) out vec3 color;
#ifdef USE_SPEC
layout(constant_id=0) const bool use_resource=false;
#endif
#ifdef USE_VS
layout(set=0,binding=0) uniform VertexData { vec4 value; } data;
layout(push_constant) uniform Push { vec4 vertex; vec4 hull; } pc;
#endif
void main() {
    gl_Position=vec4(float(gl_VertexIndex),0,0,1);
    color=vec3(1);
#ifdef USE_VS
#ifdef USE_SPEC
    if(use_resource)
#endif
    gl_Position.x+=data.value.x+pc.vertex.x;
#endif
}

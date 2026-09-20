// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(vertices=3) out;
layout(location=0) in vec3 color[];
layout(location=0) out vec3 delivered[];
#ifdef USE_SPEC
layout(constant_id=0) const bool use_resource=false;
#endif
#ifdef USE_HS
#ifdef TWO_SETS
layout(set=0,binding=0) uniform FirstData { vec4 value; } first_data;
layout(set=1,binding=0) uniform HullData { vec4 value; } data;
#else
layout(set=0,binding=1) uniform HullData { vec4 value; } data;
#endif
#ifdef DYNAMIC_PUSH
layout(push_constant) uniform Push { vec4 vertex; vec4 hull[4]; } pc;
#else
layout(push_constant) uniform Push { vec4 vertex; vec4 hull; } pc;
#endif
#endif
void main() {
    gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;
    delivered[gl_InvocationID]=color[gl_InvocationID];
#ifdef USE_HS
#ifdef USE_SPEC
    if(use_resource) {
#endif
#ifdef DYNAMIC_PUSH
#ifdef DYNAMIC_UNBOUNDED
    delivered[gl_InvocationID]+=data.value.xyz+pc.hull[int(data.value.w)].xyz;
#else
    delivered[gl_InvocationID]+=data.value.xyz+pc.hull[int(data.value.w) & 3].xyz;
#endif
#else
    delivered[gl_InvocationID]+=data.value.xyz+pc.hull.xyz;
#endif
#ifdef TWO_SETS
    delivered[gl_InvocationID]+=first_data.value.xyz;
#endif
#ifdef USE_SPEC
    }
#endif
#endif
    if(gl_InvocationID==0) {
        gl_TessLevelOuter[0]=2;gl_TessLevelOuter[1]=2;
        gl_TessLevelOuter[2]=2;gl_TessLevelInner[0]=1;
    }
}

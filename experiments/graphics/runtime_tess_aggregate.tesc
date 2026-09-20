#version 450
// Own compiler fixture: preserve typed cross-invocation IO in both namespaces.
#ifndef VALUE_TYPE
#define VALUE_TYPE mat4x3
#endif
#ifndef SECOND_LOCATION
#define SECOND_LOCATION 12
#endif
#ifdef PER_VERTEX
#define QUALIFIER
#else
#define QUALIFIER patch
#endif
layout(vertices=3) out;
layout(location=0) QUALIFIER out VALUE_TYPE scratch_values[3];
layout(location=SECOND_LOCATION) QUALIFIER out VALUE_TYPE linked_values[3];
void main() {
    scratch_values[gl_InvocationID]=VALUE_TYPE(gl_InvocationID+1);
    barrier();
    linked_values[gl_InvocationID]=scratch_values[(gl_InvocationID+2)%3];
    gl_TessLevelOuter[0]=2;
    gl_TessLevelOuter[1]=2;
    gl_TessLevelOuter[2]=2;
    gl_TessLevelInner[0]=2;
}

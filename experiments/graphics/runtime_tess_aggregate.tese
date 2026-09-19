#version 450
#ifndef VALUE_TYPE
#define VALUE_TYPE mat4x3
#endif
#ifndef SECOND_LOCATION
#define SECOND_LOCATION 12
#endif
#ifdef PER_VERTEX
#define QUALIFIER
#define INPUT_COUNT
#else
#define QUALIFIER patch
#define INPUT_COUNT 3
#endif
layout(triangles,equal_spacing,ccw) in;
layout(location=SECOND_LOCATION) QUALIFIER in VALUE_TYPE linked_values[INPUT_COUNT];
layout(location=0) out vec3 color;
void main() {
    bool valid=true;
    for(int i=0;i<3;++i)
        if(linked_values[i]!=VALUE_TYPE((i+2)%3+1))valid=false;
    gl_Position=vec4(gl_TessCoord.xy*1.8-0.9,0,1);
    color=valid?vec3(0,1,0):vec3(1,0,0);
}

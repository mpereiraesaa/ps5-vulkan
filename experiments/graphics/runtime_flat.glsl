#version 450
#ifdef TEST_VERTEX
#define DIRECTION out
#else
#define DIRECTION in
#endif
layout(location=0) DIRECTION vec2 smooth_value;
layout(location=1) flat DIRECTION float flat_float;
layout(location=2) flat DIRECTION int flat_int;
layout(location=3) flat DIRECTION uvec2 flat_uint;
#ifdef TEST_VERTEX
void main()
{
    const vec2 p[3]=vec2[3](vec2(-0.75,-0.75),vec2(0.75,-0.75),vec2(0,0.75));
    int i=gl_VertexIndex%3;
    gl_Position=vec4(p[i],0.5,1);
    smooth_value=p[i];
    flat_float=float(i)*0.125;
    flat_int=i+2;
    flat_uint=uvec2(uint(i)+3u,uint(i)+5u);
}
#else
layout(location=0) out vec4 output_color;
void main()
{
    output_color=vec4(flat_float+smooth_value.x*0.125,
        float(flat_int)*0.125+smooth_value.y*0.125,
        float(flat_uint.x+flat_uint.y)*0.0625,1);
}
#endif

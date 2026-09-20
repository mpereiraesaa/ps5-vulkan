#version 310 es
layout(location=0) in highp vec4 in_color;
layout(location=0) out mediump vec4 out_color;
void main() { out_color=in_color; }

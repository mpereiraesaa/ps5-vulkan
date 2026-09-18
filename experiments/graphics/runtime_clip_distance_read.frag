#version 450
/* The pixel end of the clip-distance interface: this stage READS the distance
 * its predecessor exported. It exists so the host contract can separate "the
 * interface policy accepts a legal pixel read, bounded by what the pre-raster
 * stage exports" from "this profile can deliver it", which needs the compiler
 * to describe the distance attribute. */
in float gl_ClipDistance[2];
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(color * gl_ClipDistance[0], 1.0);
}

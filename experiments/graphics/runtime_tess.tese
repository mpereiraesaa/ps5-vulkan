#version 450
/* Tessellation evaluation stage: triangles with equal spacing and the Vulkan
 * default winding, interpolating the per-vertex varying and consuming the
 * per-patch value the control stage wrote. */
layout(triangles, equal_spacing, cw) in;
layout(location = 0) in vec3 color[];
layout(location = 0) out vec3 out_color;
layout(location = 1) patch in float patch_tag;
void main()
{
    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position +
                  gl_TessCoord.y * gl_in[1].gl_Position +
                  gl_TessCoord.z * gl_in[2].gl_Position;
    out_color = gl_TessCoord.x * color[0] +
                gl_TessCoord.y * color[1] +
                gl_TessCoord.z * color[2] + vec3(patch_tag * 0.0);
}

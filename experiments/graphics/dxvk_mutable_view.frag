#version 450
// One nearest sample per pixel of a 16x16 texture at its texel centre; the
// sampler's view format decides whether the bytes are read as UNORM or SRGB.
layout(set=0, binding=0) uniform sampler2D mutable_view_texture;
layout(location=0) out vec4 color;
void main()
{
    color = textureLod(mutable_view_texture, gl_FragCoord.xy / vec2(16.0), 0.0);
}

#version 450
/* DXVK's pixel-shader view set (set 0, FsViews) in its in-set order: the
 * SAMPLER s0 sorts first, then the SAMPLED_IMAGE t0, then a Buffer<> SRV as
 * a UNIFORM_TEXEL_BUFFER. The texture is sampled through OpSampledImage built
 * at the use, exactly as the DXBC translation emits it. */
layout(set=0,binding=0) uniform sampler s0;
layout(set=0,binding=1) uniform texture2D t0;
layout(set=0,binding=2) uniform samplerBuffer b0;
layout(location=0) out vec4 color;
void main()
{
    vec2 uv=gl_FragCoord.xy/vec2(8.0,4.0);
    color=texture(sampler2D(t0,s0),uv)+texelFetch(b0,int(gl_FragCoord.x));
}

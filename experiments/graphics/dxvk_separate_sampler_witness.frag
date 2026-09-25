#version 450
/* DXVK's pixel-shader view set: s0 (SAMPLER) at binding 0, t0 (SAMPLED_IMAGE)
 * at 1 and a Buffer<uint> SRV (UNIFORM_TEXEL_BUFFER) at 2. Each pixel samples
 * its own texel NEAREST; alpha carries the low byte of the texel-buffer value
 * of its column, so every output byte is exact. */
layout(set=0,binding=0) uniform sampler s0;
layout(set=0,binding=1) uniform texture2D t0;
layout(set=0,binding=2) uniform utextureBuffer u0;
layout(location=0) out vec4 color;
void main()
{
    vec2 size=vec2(textureSize(sampler2D(t0,s0),0));
    vec4 texel=texture(sampler2D(t0,s0),gl_FragCoord.xy/size);
    uint low=texelFetch(u0,int(gl_FragCoord.x)).x&255u;
    color=vec4(texel.rgb,float(low)/255.0);
}

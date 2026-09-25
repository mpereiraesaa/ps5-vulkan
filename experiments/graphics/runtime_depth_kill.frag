#version 450
/* A depth-only fragment stage that removes a checkerboard of pixels and writes
 * no colour: the alpha-tested shadow-map shape. One source, three SPIR-V forms
 * of the removal, selected at compile time:
 *   default            discard, Vulkan 1.0 target  -> OpKill
 *   default, vulkan1.3 discard, SPIR-V 1.6          -> OpTerminateInvocation
 *                      (DXVK 2.6.2's internal meta shaders are this form)
 *   -DDEMOTE=1         demote, SPIR-V 1.6          -> OpDemoteToHelperInvocation
 *                      (DXVK 2.6.2's DXBC discard_nz/discard_z form)
 * Every form must remove the same pixels from the depth and stencil planes. */
#if defined(DEMOTE) && DEMOTE
#extension GL_EXT_demote_to_helper_invocation : require
#endif

void main(void)
{
    const ivec2 coord = ivec2(gl_FragCoord.xy);
    if (((coord.x ^ coord.y) & 1) != 0) {
#if defined(DEMOTE) && DEMOTE
        demote;
#else
        discard;
#endif
    }
}

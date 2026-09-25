#version 450
/* Helper-invocation derivatives after a pixel removal. The top-left pixel of
 * every 2x2 quad ((x | y) & 1 == 0) is removed BEFORE the quad computes
 * derivatives of a = x*x and b = y*y, so its three neighbours' dFdx/dFdy read
 * the removed lane's value:
 *   dFdx(a) = 2*(x & ~1) + 1, dFdy(b) = 2*(y & ~1) + 1, dFdy(a) = 0
 * (the same for coarse and fine derivatives, because a varies only with x and
 * b only with y). A demoted lane keeps executing as a helper, so the three
 * written pixels are exact; a terminated lane leaves them undefined.
 * One source, selected at compile time:
 *   default                discard  (OpKill, or OpTerminateInvocation on 1.6)
 *   -DDEMOTE=1             demote   (OpDemoteToHelperInvocation[EXT])
 *   -DTERMINATE_KHR=1      terminateInvocation (SPV_KHR_terminate_invocation)
 *   -DREMOVE=0             no removal: the control */
#if defined(DEMOTE) && DEMOTE
#extension GL_EXT_demote_to_helper_invocation : require
#endif
#if defined(TERMINATE_KHR) && TERMINATE_KHR
#extension GL_EXT_terminate_invocation : require
#endif
#ifndef REMOVE
#define REMOVE 1
#endif

layout(location = 0) out vec4 color;

void main(void)
{
    const ivec2 coord = ivec2(gl_FragCoord.xy);
    const float a = float(coord.x * coord.x);
    const float b = float(coord.y * coord.y);
#if REMOVE
    if (((coord.x | coord.y) & 1) == 0) {
#if defined(DEMOTE) && DEMOTE
        demote;
#elif defined(TERMINATE_KHR) && TERMINATE_KHR
        terminateInvocation;
#else
        discard;
#endif
    }
#endif
    color = vec4(dFdx(a) / 255.0, dFdy(b) / 255.0, dFdy(a) / 255.0, 1.0);
}

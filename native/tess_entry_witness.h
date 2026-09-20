#ifndef PS5VK_TESS_ENTRY_WITNESS_H
#define PS5VK_TESS_ENTRY_WITNESS_H
#include <stdint.h>
#include <stddef.h>

/* Diagnostic only, own shaders. Destinations derive from the compiler-described
 * user pair, never a pointer from the captured system block. Padding keeps the
 * original code's alignment and relative-PC layout. Register-preservation scope
 * is documented per helper: the trace has scratch SGPRs; the snapshot does not. */
#define PS5VK_TESS_ENTRY_PREFIX_BYTES 256u
/* Diagnostic trace: table entry7 points to an owned 8192B buffer. Slots are
 * indexed by first-active-lane v2 modulo128, NOT an invocation counter.
 * Preserve s0:23, all VGPRs and EXEC; scratch s60:67 holds no entry arguments.
 * Only for the GFX1013 merged hull ABI (system8 + user<=16). Scalar stores use
 * the encoding already exercised by the single-entry native witness. */
static inline int ps5vk_tess_hull_trace_prefix(uint32_t *out,size_t bytes,
    unsigned user_slot,unsigned user_count)
{
    if(!out || bytes<PS5VK_TESS_ENTRY_PREFIX_BYTES || user_count>16 ||
       user_slot>=16 || user_slot+1>=user_count || (user_slot&1))return -1;
    const uint32_t words[]={
        0xf4040f00u | ((8u+user_slot)/2u),0xfa000070u,
        0xbf8cc07fu,0x7e7c0502u,0xbf800007u,0x873eff3eu,127u,0x8f3e863eu,
        0x803c3e3cu,0x823d803du,
        0xf448001eu,0xfa000000u,0xf448011eu,0xfa000010u,
        0x7e800500u,0x7e820501u,0x7e840502u,0x7e860503u,
        0xbf800007u,0xf448101eu,0xfa000020u,0xbf8cc07fu
    };
    for(unsigned i=0;i<64;++i)out[i]=0xbf800000u;
    for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);++i)out[i]=words[i];
    return 0;
}
/* Single snapshot preserves every SGPR and EXEC (multiple waves overwrite it). */
static inline int ps5vk_tess_entry_prefix(uint32_t *out, size_t bytes,
    unsigned user_slot, unsigned user_count)
{
    if (!out || bytes < PS5VK_TESS_ENTRY_PREFIX_BYTES || user_count > 16 ||
        user_slot >= 16 || user_slot + 1 >= user_count || (user_slot & 1))
        return -1;
    const uint32_t sbase = (8u + user_slot) / 2u;
    for (unsigned i=0;i<64;++i) out[i]=0xbf800000u; /* s_nop 0 */
    out[0]=0xf4480000u | sbase; /* s_store_dwordx4 s[0:3], destination, 192 */
    out[1]=0xfa0000c0u;
    out[2]=0xf4480100u | sbase; /* s_store_dwordx4 s[4:7], destination, 208 */
    out[3]=0xfa0000d0u;
    out[4]=0xbf8cc07fu; /* s_waitcnt lgkmcnt(0) */
    return 0;
}
#endif

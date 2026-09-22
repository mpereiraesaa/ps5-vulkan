#ifndef PS5VK_BLEND_PS5_H
#define PS5VK_BLEND_PS5_H
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* GFX1013 encoding from the pinned Mesa src/amd/registers/gfx103.json:
 * CB_BLEND0_CONTROL, BlendOp, CombFunc and SX_MRT0_BLEND_OPT. These are
 * register words only, not evidence of hardware support or a feature gate.
 * The four SRC1 encodings are part of this register contract, but remain
 * unreachable from a Vulkan pipeline until the compiler-produced secondary
 * export and the dualSrcBlend device feature are both validated. */
struct ps5vk_blend_words {
    uint32_t control, optimization, constants[4];
};
/* Matching compiler export/target conversion, not an inherited init default.
 * Mesa ac_choose_spi_color_formats and ac_set_sx_downconvert_state_for_mrt.
 * This profile has one RGBA8/BGRA8 UNORM target. Reject unknown contracts. */
static inline int ps5vk_color_export_state(VkFormat format,uint32_t spi_format,
    uint32_t shader_mask,VkBool32 blending,VkBool32 dual_source,uint32_t words[3])
{
    if(!words)return 0;
    words[0]=words[1]=words[2]=0;
    if((blending!=VK_FALSE && blending!=VK_TRUE) ||
       (dual_source!=VK_FALSE && dual_source!=VK_TRUE))return 0;
    /* A DEPTH-ONLY pass has no colour target and its fragment program exports
     * nothing, so SPI_SHADER_COL_FORMAT is zero. There is nothing to convert:
     * the three downconversion words stay zero, which is also what a colour
     * draw must leave behind when it stops exporting. The four conditions are
     * checked together, so an exporting program without a target, or a target
     * without an export, is still refused. */
    if(format==VK_FORMAT_UNDEFINED)
        return !spi_format && !shader_mask && !blending && !dual_source;
    if(format!=VK_FORMAT_R8G8B8A8_UNORM && format!=VK_FORMAT_B8G8R8A8_UNORM)return 0;
    /* A pixel shader whose only reachable side effect is an SSBO store may
     * legally export no colour (for example, a colour store after OpKill is
     * unreachable).  PSBC reports that exact shape as both
     * SPI_SHADER_COL_FORMAT=0 and CB_SHADER_MASK=0.  It needs no target
     * conversion, and blending cannot consume a missing source.  Require the
     * pair so neither an incomplete compiler package nor an arbitrary zero
     * register is accepted on its own. */
    if(!spi_format && !shader_mask && !blending && !dual_source)return 1;
    if(dual_source) {
        /* Dual-source uses logical MRT0/MRT1 exports for one physical MRT0.
         * The target's downconversion therefore remains the MRT0 RGBA8
         * conversion; the compiler pair proves both source exports. */
        if(spi_format!=0x44u || shader_mask!=0xffu)return 0;
        words[0]=5;words[1]=6;return 1;
    }
    if(shader_mask!=15)return 0;
    if(spi_format==4) {words[0]=5;words[1]=6;return 1;}
    if(spi_format==9 && !blending) {words[0]=1;return 1;}
    return 0;
}
/* The three SX words are GLOBAL registers whose fields are per-MRT, so a
 * second target's conversion is encoded into the same words rather than
 * written as new ones. Each target's words are computed above as if it were
 * MRT0; the pinned gfx103 field layout places them at four bits per MRT for
 * SX_PS_DOWNCONVERT (MRTn at bits [4n, 4n+3]) and SX_BLEND_OPT_EPSILON, and
 * two bits per MRT for SX_BLEND_OPT_CONTROL (MRTn at bits 4n and 4n+1). One
 * target reproduces exactly the values this profile has always emitted. */
static inline void ps5vk_color_export_compose(const uint32_t per_target[][3],
    uint32_t count,uint32_t words[3])
{
    if(!words)return;
    words[0]=words[1]=words[2]=0;
    for(uint32_t target=0;target<count;++target) {
        words[0]|=(per_target[target][0]&0xfu)<<(4u*target);
        words[1]|=(per_target[target][1]&0xfu)<<(4u*target);
        words[2]|=(per_target[target][2]&0x3u)<<(4u*target);
    }
}
static inline int ps5vk_blend_factor(VkBlendFactor f,uint32_t *out)
{
    switch(f) {
    case VK_BLEND_FACTOR_ZERO:*out=0;break;
    case VK_BLEND_FACTOR_ONE:*out=1;break;
    case VK_BLEND_FACTOR_SRC_COLOR:*out=2;break;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:*out=3;break;
    case VK_BLEND_FACTOR_SRC_ALPHA:*out=4;break;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:*out=5;break;
    case VK_BLEND_FACTOR_DST_ALPHA:*out=6;break;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:*out=7;break;
    case VK_BLEND_FACTOR_DST_COLOR:*out=8;break;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:*out=9;break;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:*out=10;break;
    case VK_BLEND_FACTOR_CONSTANT_COLOR:*out=13;break;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:*out=14;break;
    case VK_BLEND_FACTOR_SRC1_COLOR:*out=15;break;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:*out=16;break;
    case VK_BLEND_FACTOR_SRC1_ALPHA:*out=17;break;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:*out=18;break;
    case VK_BLEND_FACTOR_CONSTANT_ALPHA:*out=19;break;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:*out=20;break;
    default:return 0;
    }
    return 1;
}
static inline int ps5vk_blend_equation(VkBlendOp op,VkBlendFactor src,
    VkBlendFactor dst,uint32_t *s,uint32_t *d,uint32_t *fn)
{
    switch(op) {
    case VK_BLEND_OP_ADD:*fn=0;break;
    case VK_BLEND_OP_SUBTRACT:*fn=1;break;
    case VK_BLEND_OP_REVERSE_SUBTRACT:*fn=4;break;
    /* Vulkan MIN/MAX ignore factors; normalize as RADV does. */
    case VK_BLEND_OP_MIN:*fn=2;*s=*d=1;return 1;
    case VK_BLEND_OP_MAX:*fn=3;*s=*d=1;return 1;
    default:return 0;
    }
    return ps5vk_blend_factor(src,s) && ps5vk_blend_factor(dst,d);
}
static inline int ps5vk_blend_factor_uses_src1(VkBlendFactor factor)
{
    return factor==VK_BLEND_FACTOR_SRC1_COLOR ||
        factor==VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
        factor==VK_BLEND_FACTOR_SRC1_ALPHA ||
        factor==VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}
static inline int ps5vk_blend_encode(const VkPipelineColorBlendAttachmentState *a,
    const float constants[4],VkBool32 dual_source,struct ps5vk_blend_words *out)
{
    if(!out)return 0;
    memset(out,0,sizeof(*out));
    if(!a || (dual_source!=VK_FALSE && dual_source!=VK_TRUE))return 0;
    /* No RB+ algebraic shortcuts: preserve both operands, no combining
     * optimization. Emit explicitly even for disabled blending on switches. */
    struct ps5vk_blend_words w={.optimization=0x00770077u};
    if(!a->blendEnable){*out=w;return 1;}
    if(a->blendEnable!=VK_TRUE || !constants)return 0;
    if(!dual_source &&
       (ps5vk_blend_factor_uses_src1(a->srcColorBlendFactor) ||
        ps5vk_blend_factor_uses_src1(a->dstColorBlendFactor) ||
        ps5vk_blend_factor_uses_src1(a->srcAlphaBlendFactor) ||
        ps5vk_blend_factor_uses_src1(a->dstAlphaBlendFactor)))return 0;
    uint32_t s,d,fn,sa,da,fna;
    if(!ps5vk_blend_equation(a->colorBlendOp,a->srcColorBlendFactor,
        a->dstColorBlendFactor,&s,&d,&fn) ||
       !ps5vk_blend_equation(a->alphaBlendOp,a->srcAlphaBlendFactor,
        a->dstAlphaBlendFactor,&sa,&da,&fna))return 0;
    w.control=(1u<<30)|s|(fn<<5)|(d<<8);
    if(s!=sa || d!=da || fn!=fna)
        w.control|=(1u<<29)|(sa<<16)|(fna<<21)|(da<<24);
    memcpy(w.constants,constants,sizeof(w.constants));
    *out=w;return 1;
}
#endif

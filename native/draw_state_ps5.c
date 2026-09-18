#include "draw_state_ps5.h"
#include "viewport_ps5.h"
#include <string.h>
VkResult ps5vk_native_draw_state(VkPipeline p, const VkViewport *viewport_state,
    const VkRect2D *scissor_state, const struct ps5vk_target_registers *color,
    const struct ps5vk_target_registers *depth, const VkRect2D *area,
    uint32_t width, uint32_t height, unsigned index_width, struct ps5vk_draw_state *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (!p || !viewport_state || !scissor_state || !p->graphics || !p->graphics_state || !color || color->count != 16 ||
        !width || !height || width > 16384 || height > 16384 ||
        (p->color_format != VK_FORMAT_B8G8R8A8_UNORM &&
         p->color_format != VK_FORMAT_R8G8B8A8_UNORM) ||
        (p->cull_mode & ~VK_CULL_MODE_FRONT_AND_BACK) ||
        (p->front_face != VK_FRONT_FACE_CLOCKWISE && p->front_face != VK_FRONT_FACE_COUNTER_CLOCKWISE) ||
        p->depth_compare > VK_COMPARE_OP_ALWAYS || p->depth_compare < VK_COMPARE_OP_NEVER)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (p->depth_format == VK_FORMAT_UNDEFINED ? depth != NULL :
        (p->depth_format != VK_FORMAT_D32_SFLOAT || !depth || depth->count != PS5_DEPTH_REGISTER_COUNT))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_native_graphics_pipeline *native = p->graphics_state;
    if (native->device != p->device || !native->pair || !native->pair->ready) return VK_ERROR_UNKNOWN;
    struct ps5vk_graphics_pair *pair = native->pair;
    int runtime=pair->runtime_arguments.enabled!=0;
    /* A tessellation pipeline's pre-raster bank is the domain half's, and its
     * draw is a patch list; anything else is a broken pair. */
    const int has_tessellation=pair->tessellation!=0;
    if(has_tessellation && !runtime)return VK_ERROR_UNKNOWN;
    const struct ps5vk_runtime_shader *vs=&pair->runtime_vertex,*fs=&pair->runtime_fragment;
    if(runtime && (vs->header.num_cx_registers>PS5VK_RUNTIME_CX_MAX ||
        fs->header.num_cx_registers>PS5VK_RUNTIME_CX_MAX ||
        !vs->header.num_sh_registers || !fs->header.num_sh_registers ||
        vs->header.num_sh_registers>PS5VK_RUNTIME_SH_MAX ||
        fs->header.num_sh_registers>PS5VK_RUNTIME_SH_MAX ||
        (has_tessellation &&
            (pair->runtime_hull_ls.header.num_sh_registers>4 ||
             pair->runtime_hull_hs.header.num_sh_registers>4 ||
             pair->runtime_hull_hs.header.num_cx_registers>PS5VK_RUNTIME_CX_MAX))))
        return VK_ERROR_UNKNOWN;
    if (pair->vertex_quantization != 0x2d) return VK_ERROR_FEATURE_NOT_PRESENT;
    ps5_agc_register viewport[PS5VK_VIEWPORT_REGISTERS];
    VkResult rc = ps5vk_native_viewport(viewport_state, scissor_state, area, viewport);
    if (rc != VK_SUCCESS) return rc;
    struct ps5_pipeline_registers base;
    if (ps5_pipeline_build(&base, color->registers, &pair->cx, &pair->uc,
        runtime?vs->context:pair->gs.cx, runtime?fs->context:pair->ps.cx,
        runtime?vs->shader:pair->gs.sh,runtime?fs->shader:pair->ps.sh,width,height)) return VK_ERROR_UNKNOWN;
    for (unsigned j = 0; j < PS5VK_VIEWPORT_REGISTERS; ++j) {
        if(viewport[j].offset==0x094 || viewport[j].offset==0x095)continue;
        unsigned replaced = 0;
        for (unsigned k = 16; k < 31; ++k) if (base.cx[k].offset == viewport[j].offset) {
            base.cx[k] = viewport[j]; ++replaced;
        }
        if (replaced != 1) return VK_ERROR_UNKNOWN;
    }
    struct ps5vk_draw_state result = {0};
    memcpy(result.cx, base.cx, sizeof(base.cx)); result.cx_count = PS5_PIPELINE_CX_REGISTERS;
    if(runtime) {
        result.cx_count=PS5_PIPELINE_RT_REGISTERS+PS5_PIPELINE_VIEWPORT_REGISTERS+PS5_PIPELINE_LINKED_CX_REGISTERS;
        memcpy(result.cx+result.cx_count,vs->context,vs->header.num_cx_registers*sizeof(*result.cx));
        result.cx_count+=vs->header.num_cx_registers;
        memcpy(result.cx+result.cx_count,fs->context,fs->header.num_cx_registers*sizeof(*result.cx));
        result.cx_count+=fs->header.num_cx_registers;
    }
    if (depth) {
        memcpy(result.cx + result.cx_count, depth->registers, depth->count * sizeof(ps5_agc_register));
        result.cx_count += depth->count;
    }
    /* The tessellation pair's hull launch state and the hull programs' own
     * register blocks join the context/shader banks: the stage enables and the
     * LS_HS_CONFIG the driver derived at create, the hull HS's context block
     * (which carries VGT_TF_PARAM) and both programs' shader blocks. The
     * context registers carry VGT_TF_PARAM; the ring configuration is
     * program-referenced state below. */
    if (has_tessellation) {
        if (result.cx_count+2+pair->runtime_hull_hs.header.num_cx_registers >
            PS5VK_DRAW_CX_CAPACITY)
            return VK_ERROR_UNKNOWN;
        memcpy(result.cx+result.cx_count,pair->tess_state,
            sizeof(pair->tess_state));
        result.cx_count+=2;
        memcpy(result.cx+result.cx_count,pair->runtime_hull_hs.context,
            pair->runtime_hull_hs.header.num_cx_registers*sizeof(*result.cx));
        result.cx_count+=pair->runtime_hull_hs.header.num_cx_registers;
    }
    /* Override the depth builder's Gears policy. Vulkan disables writes when
     * depth testing is disabled, even if depthWriteEnable was specified. */
    uint32_t depth_control = depth && p->depth_test ?
        2u | (p->depth_write ? 4u : 0u) | ((uint32_t)p->depth_compare << 4) : 0u;
    result.cx[result.cx_count++] = (ps5_agc_register){0x200, depth_control};
    /* Public Mesa gfx10/RADV: filled triangles, first provoking vertex. */
    result.cx[result.cx_count++] = (ps5_agc_register){0x205,
        (uint32_t)p->cull_mode | ((uint32_t)p->front_face << 2) | (2u << 5) | (2u << 8)};
    /* Shader exports homogeneous W, not reciprocal W. Mesa RADV and the
     * compiler's .pa_cl_vte_cntl.vtx_w0_fmt both require this bit. w=1 tests
     * cannot distinguish the two modes. */
    result.cx[result.cx_count++] = (ps5_agc_register){0x206, 0x43f};
    /* Explicit single-sample filled-triangle state, matching RADV gfx10:
     * VPORT_SCISSOR_ENABLE and ALTERNATE_RBS_PER_TILE; no MSAA/line stipple.
     * Keep the generic scissor at the target bounds and apply the Vulkan
     * scissor/render-area intersection to viewport zero. */
    result.cx[result.cx_count++] = (ps5_agc_register){0x292, 0x22};
    result.cx[result.cx_count++] = viewport[8];
    result.cx[result.cx_count++] = viewport[9];
    /* Mesa gfx10 PA_CL_CLIP_CNTL: Vulkan's default 0 <= z <= w clip
     * volume and linear attribute clipping. Depth clamp, negative-one-to-one
     * depth and rasterizer discard are not supported by this profile. */
    result.cx[result.cx_count++] = (ps5_agc_register){0x204, (1u << 19) | (1u << 24)};
    /* Compiler PAL PA_SU_VTX_CNTL, packed with the pinned Mesa schema:
     * pixel center 1, round-to-even 2, 1/256 quantization mode 5. Do not rely
     * on an inherited AGC initialization value for rasterization precision. */
    result.cx[result.cx_count++] = (ps5_agc_register){0x2f9, pair->vertex_quantization};
    uint32_t restart_enable=0;
    /* Primitive restart, at the front end that acts on it. On GFX10 the ENABLE is
     * a UCONFIG register (0x3092c, written with the other user-config registers
     * below) while the index the hardware compares against is a CONTEXT register
     * (0x2840c); RADV programs exactly that pair for gfx_level >= GFX9 and
     * gfx_level < GFX11, with the index the bound index type implies (0xffff for
     * UINT16, 0xffffffff for UINT32) and no MATCH_ALL_BITS, whose default already
     * compares only the index's own bits. A non-indexed draw carries no restart
     * index, so the enable stays clear and a stale value cannot cut anything. */
    {
        const int indexed = index_width != 0;
        if (index_width != 0 && index_width != 2 && index_width != 4)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        const int restart = p->primitive_restart && indexed;
        result.cx[result.cx_count++] = (ps5_agc_register){0x103,
            restart ? (index_width == 2 ? 0xffffu : 0xffffffffu) : 0u};
        /* GFX10 has a known synchronisation bug when the primitive-restart state
         * is updated and no context register is written between draws; Mesa
         * emits a SQ_NON_EVENT NOP before the update, and so does this path when
         * the update happens. */
        if (restart)
            result.cx[result.cx_count++] = (ps5_agc_register){0x2a4, 38u};
        restart_enable = restart ? 1u : 0u;
    }
    memcpy(result.sh, base.sh, sizeof(base.sh)); memcpy(result.uc, base.uc, sizeof(base.uc));
    /* The linked UC block is ge_cntl, user-VGPR enable and the primitive type;
     * nothing else in this profile's state is a UC register today. */
    result.uc_count=3;
    /* The enable lives in the user-config block on GFX9-GFX10: R_03092C named
     * VGT_MULTI_PRIM_IB_RESET_EN there, reached with the same index space the
     * linked UC block and the index-type packet use, (address - 0x30000)/4. */
    result.uc[result.uc_count++]=(ps5_agc_register){0x24b,restart_enable};
    /* The tessellation rings are program-referenced device state: the whole
     * configuration is written on every patch draw so nothing depends on what
     * another pipeline left configured. */
    if(has_tessellation) {
        if(result.uc_count+4>PS5VK_DRAW_UC_CAPACITY)return VK_ERROR_UNKNOWN;
        memcpy(result.uc+result.uc_count,pair->tess_ring_state,
            sizeof(pair->tess_ring_state));
        result.uc_count+=4;
    }
    result.modifier = pair->gs.specials.draw_modifier;
    if(runtime) {
        result.sh_count=vs->header.num_sh_registers+fs->header.num_sh_registers;
        memcpy(result.sh,vs->shader,vs->header.num_sh_registers*sizeof(*result.sh));
        memcpy(result.sh+vs->header.num_sh_registers,fs->shader,fs->header.num_sh_registers*sizeof(*result.sh));
        /* The hull programs' shader blocks follow the runtime pair's: the HS
         * program (its VGT_TF_PARAM context block went into the context bank)
         * and the LS program behind it, both with the create-path addresses. */
        if(has_tessellation) {
            if(result.sh_count+pair->runtime_hull_ls.header.num_sh_registers+
                pair->runtime_hull_hs.header.num_sh_registers>PS5VK_DRAW_SH_CAPACITY)
                return VK_ERROR_UNKNOWN;
            memcpy(result.sh+result.sh_count,pair->runtime_hull_hs.shader,
                pair->runtime_hull_hs.header.num_sh_registers*sizeof(*result.sh));
            result.sh_count+=pair->runtime_hull_hs.header.num_sh_registers;
            memcpy(result.sh+result.sh_count,pair->runtime_hull_ls.shader,
                pair->runtime_hull_ls.header.num_sh_registers*sizeof(*result.sh));
            result.sh_count+=pair->runtime_hull_ls.header.num_sh_registers;
        }
        result.runtime=pair->runtime_arguments;
        /* This is the lab's audited draw-auto command modifier, not compiler
         * metadata. PSBC headers do not populate the legacy PAL field. */
        result.modifier=5;
    }
    if (!result.modifier) return VK_ERROR_UNKNOWN;
    *out = result; return VK_SUCCESS;
}

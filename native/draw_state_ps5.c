#include "draw_state_ps5.h"
#include "viewport_ps5.h"
#include "blend_ps5.h"
#include <string.h>
#if defined(PS5VK_TESS_STATE_DUMP) && PS5VK_TESS_STATE_DUMP
#include "ps5log.h"
/* The complete register set a patch draw actually emits, logged once.
 *
 * Every tessellation candidate so far has been argued from ONE register read
 * out of the object that holds it. That cannot see the two failure modes the
 * emitted stream can still have after every individual value is right: a
 * register the pipeline never writes at all, and a register written TWICE
 * where the later write wins. The banks are emitted in order and the last
 * write for an offset is the one the engine sees, so printing the stream in
 * emission order is the only way to read either off. Four registers per
 * record, with no string formatting of its own, so the dump cannot itself
 * fail on a buffer bound. */
static void tess_dump_bank(const char *bank,const ps5_agc_register *regs,
    unsigned count)
{
    for(unsigned i=0;i<count;i+=4) {
        const ps5_agc_register zero={0xfff,0};
        const ps5_agc_register *a=&regs[i];
        const ps5_agc_register *b=i+1<count?&regs[i+1]:&zero;
        const ps5_agc_register *c=i+2<count?&regs[i+2]:&zero;
        const ps5_agc_register *d=i+3<count?&regs[i+3]:&zero;
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_TESS_EMIT bank=%s at=%u of=%u %03x=%08x %03x=%08x "
            "%03x=%08x %03x=%08x",bank,i,count,
            (unsigned)a->offset,a->value,(unsigned)b->offset,b->value,
            (unsigned)c->offset,c->value,(unsigned)d->offset,d->value);
    }
}
#endif

static uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }
/* Public Mesa RADV (radv_emit_depth_bias_state, gfx10): the polygon offset
 * block is CLAMP, FRONT_SCALE, FRONT_OFFSET, BACK_SCALE, BACK_OFFSET at
 * 0x2df..0x2e3 with the slope scaled by 16, and PA_SU_POLY_OFFSET_DB_FMT_CNTL
 * (0x2de) describing the depth format: NEG_NUM_DB_BITS = -23 with
 * POLY_OFFSET_DB_IS_FLOAT_FMT for D32_SFLOAT, and zero when there is no depth
 * attachment (Vulkan leaves the constant term's unit undefined then). Both
 * front and back get the same scale/offset: Vulkan has one bias per polygon,
 * not per face. The factors are written even when the bias is disabled so the
 * words never carry another draw's values. */
static void polygon_offset(const struct ps5vk_raster_state *raster, int depth_d32,
    ps5_agc_register out[6])
{
    const uint32_t slope = float_bits(raster->depth_bias_slope * 16.0f);
    const uint32_t offset = float_bits(raster->depth_bias_constant);
    out[0] = (ps5_agc_register){0x2de, depth_d32 ? ((uint32_t)(-23) & 0xffu) | (1u << 8) : 0u};
    out[1] = (ps5_agc_register){0x2df, float_bits(raster->depth_bias_clamp)};
    out[2] = (ps5_agc_register){0x2e0, slope};
    out[3] = (ps5_agc_register){0x2e1, offset};
    out[4] = (ps5_agc_register){0x2e2, slope};
    out[5] = (ps5_agc_register){0x2e3, offset};
}
VkResult ps5vk_native_draw_state(VkPipeline p, const VkViewport *viewport_state,
    const VkRect2D *scissor_state, uint32_t viewport_count,
    const struct ps5vk_raster_state *raster,
    const struct ps5vk_target_registers *colors, uint32_t color_count,
    const struct ps5vk_target_registers *depth, const VkRect2D *area,
    uint32_t width, uint32_t height, unsigned index_width, struct ps5vk_draw_state *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (!viewport_count || viewport_count > PS5VK_MAX_VIEWPORTS) return VK_ERROR_UNKNOWN;
    if (!p || !viewport_state || !scissor_state || !raster || !p->graphics || !p->graphics_state ||
        !width || !height || width > 16384 || height > 16384 ||
        /* The colour targets are one per attachment the pipeline was created
         * for. A count of zero is a DEPTH-ONLY pass: the pipeline for such a
         * subpass records no colour attachment and an undefined colour format,
         * so the two always agree - a colour target with an undefined format,
         * or a missing one with a real format, are both refused here. The
         * pointer itself carries no meaning at a count of zero (the caller
         * hands over its prepared-target array whatever the count is), so only
         * the count is read. */
        (p->color_attachment_count ?
            (!colors || colors[0].count != 16 ||
             (p->color_format[0] != VK_FORMAT_B8G8R8A8_UNORM &&
              p->color_format[0] != VK_FORMAT_R8G8B8A8_UNORM)) :
            (color_count || !depth || p->color_format[0] != VK_FORMAT_UNDEFINED)) ||
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
    if(pair->dual_source_export>1u || (pair->dual_source_export && !runtime))
        return VK_ERROR_UNKNOWN;
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
            (pair->runtime_hull.header.num_sh_registers>4 ||
             pair->runtime_hull.header.num_cx_registers>PS5VK_RUNTIME_CX_MAX))))
        return VK_ERROR_UNKNOWN;
    if (pair->vertex_quantization != 0x2d) return VK_ERROR_FEATURE_NOT_PRESENT;
    ps5_agc_register viewport[PS5VK_VIEWPORT_REGISTERS];
    VkResult rc = ps5vk_native_viewport(viewport_state, scissor_state, area, viewport);
    if (rc != VK_SUCCESS) return rc;
    /* The prepared colour targets, one per attachment the subpass names. The
     * count is the pipeline's own, so a draw can never programme a target the
     * pipeline was not created for. */
    if (color_count > PS5VK_MAX_COLOR_ATTACHMENTS ||
        color_count != p->color_attachment_count ||
        (color_count && !colors)) return VK_ERROR_UNKNOWN;
    struct ps5_pipeline_registers base;
    /* The render-target block comes from the pipeline's colour target. At a
     * colour count of zero - the DEPTH-ONLY shape - there is no such target
     * and the caller's prepared array is not initialised, so the builder is
     * handed nothing and emits the zeroed block itself. */
    if (ps5_pipeline_build(&base, color_count ? colors[0].registers : NULL, &pair->cx, &pair->uc,
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
    /* CB_TARGET_MASK (context offset 0x08e) is the render-target register that
     * names the colour channels target zero writes; ps5_pipeline_build places
     * it in the RT/viewport block with all four channels enabled. Carry the
     * pipeline's own mask, in that block and never in the per-draw stream - a
     * draw-stream write of this register stalled the queue when it was tried.
     * The shipping profile only ever reaches here with the all-channel mask
     * the witnesses measured; a partial mask needs the compiler to accept it
     * first. */
    {
        /* CB_TARGET_MASK carries one nibble per target: attachment zero's
         * write mask in bits [3:0] and, once the profile serves a second
         * target, the next attachment's in bits [7:4]. */
        uint32_t target_mask = 0;
        for (uint32_t attachment = 0; attachment < p->color_attachment_count; ++attachment)
            target_mask |= (p->color_blend[attachment].colorWriteMask & 0xfu) << (4u * attachment);
        unsigned carried = 0;
        for (unsigned k = 16; k < 31; ++k) if (base.cx[k].offset == 0x08e) {
            base.cx[k].value = target_mask; ++carried;
        }
        if (carried != 1) return VK_ERROR_UNKNOWN;
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
        if (result.cx_count+(unsigned)(sizeof(pair->tess_state)/
                sizeof(pair->tess_state[0]))+
            pair->runtime_hull.header.num_cx_registers >
            PS5VK_DRAW_CX_CAPACITY)
            return VK_ERROR_UNKNOWN;
        memcpy(result.cx+result.cx_count,pair->tess_state,
            sizeof(pair->tess_state));
        result.cx_count+=(unsigned)(sizeof(pair->tess_state)/
            sizeof(pair->tess_state[0]));
        /* DIAGNOSTIC, default off: omit the hull's own context block, which
         * on this profile is exactly VGT_TF_PARAM.
         *
         * This driver writes that register WHOLE, from a value psbc derives
         * from the control half's declared interface alone - domain, spacing,
         * topology - so DISABLE_DONUTS, DETECT_ONE, DETECT_ZERO and MTYPE are
         * all zeroed every patch draw. Whether that matters was a theory
         * until run 42 turned it into a mechanism: the tessellation-level
         * clamps at cx 0x286/0x287 had never been written by this driver and
         * turned out to already hold sane values, which proves the platform
         * DOES leave context registers initialised and that zeroing a field
         * destroys something real.
         *
         * Omitting the write is the only way to ask the question, because
         * there is no read-modify-write for these banks. The domain type may
         * then be wrong - but the witness measures whether the evaluation
         * half EXECUTES, not whether it produces the right picture, so a
         * wrong tessellator configuration still answers it. */
#if !(defined(PS5VK_TESS_NO_TF_PARAM) && PS5VK_TESS_NO_TF_PARAM)
        memcpy(result.cx+result.cx_count,pair->runtime_hull.context,
            pair->runtime_hull.header.num_cx_registers*sizeof(*result.cx));
        result.cx_count+=pair->runtime_hull.header.num_cx_registers;
#endif
        /* The ring descriptor table, delivered as USER DATA.
         *
         * The hull's first memory operation is an SMEM load of a buffer
         * descriptor from this table (entry 5, the tess-factor ring), so a
         * patch draw faults at any tessellation level without it. The
         * compiler assigns the window-relative dword and the driver writes
         * the 64-bit address there; the hull's user-data window on gfx10 is
         * SPI_SHADER_USER_DATA_HS_0, sh offset 0x10c.
         *
         * It is written with the SHADER bank below, not here. This block
         * runs before the shader bank exists: the runtime path assigns
         * result.sh_count from the pre-raster and fragment counts and
         * memcpy()s both programs over result.sh[0..], so two entries
         * written here were overwritten by the vertex program and the count
         * that would have carried them was reset to zero. The registers
         * were individually correct and simply never reached the engine -
         * measured, not reasoned: the full emitted stream showed the shader
         * bank holding exactly fourteen entries, the domain's six, the
         * fragment's four and the hull's four, with 0x112 and 0x113 absent.
         *
         * What is NOT written with it, deliberately.
         *
         * An earlier version wrote the ring descriptor table's address into
         * the hull's user-data window at sh 0x10c/0x10d, intending to reach
         * the hull's ring_offsets. The register mapping was right - 0x10c
         * really is SPI_SHADER_USER_DATA_HS_0 - but the DELIVERY MODEL was
         * wrong, and measurably so. The merged hull's own argument layout
         * says:
         *
         *   ring_offsets   SGPR 0-2   (system block, below the user-data
         *                              window base of 8: hardware-supplied)
         *   ud[18] AC_UD_DYNAMIC_DESCRIPTORS             SGPR 8
         *   ud[19] AC_UD_DYNAMIC_DESCRIPTORS_OFFSET_ADDR SGPR 9
         *
         * User-data dwords 0 and 1 ARE SGPRs 8 and 9, so that write never
         * reached ring_offsets at all - it overwrote the hull's two
         * dynamic-descriptor pointers. ring_offsets is supplied by the
         * hardware from the device's global ring configuration, which is why
         * every working vertex/NGG path on this device needs no such write
         * either. The rings themselves are still bound, through the VGT
         * registers the create path programs in the user-config bank. */
    }
    /* Override the depth builder's Gears policy. Vulkan disables writes when
     * depth testing is disabled, even if depthWriteEnable was specified. */
    uint32_t depth_control = depth && p->depth_test ?
        2u | (p->depth_write ? 4u : 0u) | ((uint32_t)p->depth_compare << 4) : 0u;
    result.cx[result.cx_count++] = (ps5_agc_register){0x200, depth_control};
    /* Public Mesa gfx10/RADV PA_SU_SC_MODE_CNTL: cull mode, front face,
     * first provoking vertex, the three POLY_OFFSET_*_ENABLE bits (11..13)
     * exactly when the draw's depth bias is enabled, and the polygon mode:
     * POLYMODE_FRONT/BACK_PTYPE (bits 5..7 / 8..10) name what a polygon
     * rasterizes as - 0 points, 1 lines, 2 triangles in the gfx10 schema,
     * the reverse of VkPolygonMode's FILL=0, LINE=1, POINT=2 - and POLY_MODE
     * (bit 3, DUAL_MODE) plus KEEP_TOGETHER_ENABLE (bit 24, gfx10..gfx11:
     * the SC must process the primitive group in PA order) are set for
     * either non-solid mode. Culling and facing are decided on the polygon
     * before the mode applies, so the cull bits stay as they are. */
    uint32_t hardware_polygon_type = 2u, polygon_mode = 0u;
    if (raster->polygon_mode == VK_POLYGON_MODE_LINE) hardware_polygon_type = 1u;
    else if (raster->polygon_mode == VK_POLYGON_MODE_POINT) hardware_polygon_type = 0u;
    else if (raster->polygon_mode != VK_POLYGON_MODE_FILL) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (hardware_polygon_type != 2u) polygon_mode = (1u << 3) | (1u << 24);
    const uint32_t polygon_offset_enable = raster->depth_bias_enable ?
        (1u << 11) | (1u << 12) | (1u << 13) : 0u;
    result.cx[result.cx_count++] = (ps5_agc_register){0x205,
        (uint32_t)p->cull_mode | ((uint32_t)p->front_face << 2) |
        (hardware_polygon_type << 5) | (hardware_polygon_type << 8) |
        polygon_mode | polygon_offset_enable};
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
    /* Mesa gfx10 PA_CL_CLIP_CNTL: Vulkan's default 0 <= z <= w clip volume
     * (DX_CLIP_SPACE_DEF) and linear attribute clipping. depthClampEnable
     * disables the near and far z clip planes (ZCLIP_NEAR_DISABLE,
     * ZCLIP_FAR_DISABLE), exactly as RADV's depth-clamp mode does; the clamp
     * interval itself is PA_SC_VPORT_ZMIN/ZMAX, already programmed above from
     * the viewport's ordered min/max depth, and DB_RENDER_OVERRIDE keeps the
     * viewport clamp enabled. Lateral clipping and the W semantics are
     * untouched. Negative-one-to-one depth and rasterizer discard are not
     * supported by this profile. */
    result.cx[result.cx_count++] = (ps5_agc_register){0x204, (1u << 19) | (1u << 24) |
        (raster->depth_clamp ? (1u << 26) | (1u << 27) : 0u)};
    /* Compiler PAL PA_SU_VTX_CNTL, packed with the pinned Mesa schema:
     * pixel center 1, round-to-even 2, 1/256 quantization mode 5. Do not rely
     * on an inherited AGC initialization value for rasterization precision. */
    result.cx[result.cx_count++] = (ps5_agc_register){0x2f9, pair->vertex_quantization};
    /* Depth bias: the polygon offset block, written on every draw. */
    polygon_offset(raster, depth != NULL, result.cx + result.cx_count);
    result.cx_count += 6;
    /* Point and line rasterization state for the non-solid polygon modes,
     * written on every draw so a FILL draw cannot inherit another value.
     * Public Mesa RADV: PA_SU_POINT_SIZE holds the 12.4 fixed-point point
     * width/height and PA_SU_POINT_MINMAX the clamp interval it programs in
     * its preamble (min 0, max 8191.875/2 in 12.4 = 0xffff); PA_SU_LINE_CNTL
     * WIDTH is the line width * 8 (1.0 px, wideLines is not advertised);
     * PA_SC_LINE_CNTL is zero (Bresenham lines, no perpendicular end caps).
     * In this 1.0 profile the point size of a POINT-mode polygon is 1.0
     * unless the compiler enables the shader's PointSize export in
     * PA_CL_VS_OUT_CNTL, which the spec allows either way (polygonModePointSize
     * is a maintenance5 property this profile does not report). */
    result.cx[result.cx_count++] = (ps5_agc_register){0x280, (8u << 16) | 8u};
    result.cx[result.cx_count++] = (ps5_agc_register){0x281, 0xffffu << 16};
    result.cx[result.cx_count++] = (ps5_agc_register){0x282, 8u};
    result.cx[result.cx_count++] = (ps5_agc_register){0x2f7, 0u};
    /* Viewport banks 1..count-1: their own PA_CL_VPORT, ZMIN/ZMAX and
     * VPORT_SCISSOR words, each scissor intersected with the render area like
     * bank zero's. Only the indices this draw was recorded with are written;
     * a ViewportIndex outside 0..count-1 is undefined in Vulkan and selects
     * whatever the context holds, never another draw's promise. */
    for (uint32_t i = 1; i < viewport_count; ++i) {
        rc = ps5vk_native_viewport_bank(i, &viewport_state[i], &scissor_state[i], area,
            result.cx + result.cx_count);
        if (rc != VK_SUCCESS) return rc;
        result.cx_count += PS5VK_VIEWPORT_REGISTERS;
    }
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
        {
            const unsigned ring_state_count=
                (unsigned)(sizeof(pair->tess_ring_state)/
                    sizeof(pair->tess_ring_state[0]));
            if(result.uc_count+ring_state_count>PS5VK_DRAW_UC_CAPACITY)
                return VK_ERROR_UNKNOWN;
            memcpy(result.uc+result.uc_count,pair->tess_ring_state,
                sizeof(pair->tess_ring_state));
            result.uc_count+=ring_state_count;
        }
#if defined(PS5VK_TESS_GE_CNTL) && PS5VK_TESS_GE_CNTL
        /* DIAGNOSTIC, default off: GE_CNTL programmed the way a TESSELLATION
         * draw programs it, appended after the linked value so the later write
         * wins exactly as the context bank's second VGT_SHADER_STAGES_EN does.
         *
         * The linked GE_CNTL (0x00010080 here) is the NGG VERTEX pipeline's
         * value: PRIM_GRP_SIZE = the NGG program's max primitives per subgroup
         * and VERT_GRP_SIZE = its max vertices, which is what the pinned
         * radv_shader.c publishes for an NGG stage. Both open gfx10 drivers
         * REPLACE that at draw time when a control shader is bound. PAL,
         * gfx9UniversalCmdBuffer.cpp CalcGeCntl: under tessellation
         * PRIM_GRP_SIZE = IA_MULTI_VGT_PARAM.PRIMGROUP_SIZE + 1, where
         * gfx9GraphicsPipeline.cpp SetupIaMultiVgtParam says "the hardware
         * requires that the primgroup size matches the number of HS
         * patches-per-thread-group when tessellation is enabled"
         * (gfx9Device.cpp ComputeTessPrimGroupSize: a multiple of the patch
         * count, at least 4, even on more than two shader engines);
         * VERT_GRP_SIZE = 256, the documented way to disable vertex grouping;
         * and BREAK_WAVE_AT_EOI = 1 for every tessellation draw, quoting the
         * hardware team that "every DS requires a valid PatchId".
         *
         * This register was cleared earlier as "byte-identical to a passing
         * draw". The passing draw was a vertex NGG pipeline, for which that
         * value is correct, so it was never a control for a patch draw.
         *
         * Mode 1 is PAL's rule. Mode 2 drops the vertex-grouping and
         * break-wave bits, the radeonsi form for a control shader that does
         * not read the primitive id, to separate the group size from the
         * other two fields if mode 1 changes the result.
         *
         * HISTORICAL, BEFORE WORKING NATIVE RING BINDING: mode 1 readback of GE_CNTL
         * returned 0x00060040 for this pipeline's 64 patches per workgroup,
         * the draw completed, and the evaluation half still produced nothing
         * (ink=0). That empty-draw result does not rule out a grouping defect
         * once tessellation runs. Reopened for the current cross-invocation
         * missing-patch investigation; remains diagnostic/default off. */
        {
            const uint32_t num_patches=pair->tess_state[1].value&255u;
            uint32_t prim_grp=num_patches?num_patches:4u;
            while(prim_grp<4u)prim_grp+=num_patches;
            uint32_t ge_cntl=prim_grp&0x1ffu;
#if PS5VK_TESS_GE_CNTL==1
            ge_cntl|=(256u<<9)|(1u<<18);
#endif
            if(result.uc_count+1>PS5VK_DRAW_UC_CAPACITY)return VK_ERROR_UNKNOWN;
            result.uc[result.uc_count++]=(ps5_agc_register){0x25b,ge_cntl};
        }
#endif
    }
    result.modifier = pair->gs.specials.draw_modifier;
    if(runtime) {
        result.sh_count=vs->header.num_sh_registers+fs->header.num_sh_registers;
        memcpy(result.sh,vs->shader,vs->header.num_sh_registers*sizeof(*result.sh));
        memcpy(result.sh+vs->header.num_sh_registers,fs->shader,fs->header.num_sh_registers*sizeof(*result.sh));
        /* The merged hull program's one shader block follows the runtime
         * pair's: its address register at the LS block and its resource pair
         * at the HS block, with the create-path address. Its VGT_TF_PARAM
         * context block went into the context bank. */
        if(has_tessellation) {
            if(result.sh_count+pair->runtime_hull.header.num_sh_registers>
                PS5VK_DRAW_SH_CAPACITY)
                return VK_ERROR_UNKNOWN;
            memcpy(result.sh+result.sh_count,pair->runtime_hull.shader,
                pair->runtime_hull.header.num_sh_registers*sizeof(*result.sh));
            result.sh_count+=pair->runtime_hull.header.num_sh_registers;
            /* The ring descriptor table's address, as USER DATA, appended to
             * the bank that actually reaches the engine. The hull's first
             * memory operation is an SMEM load of a buffer descriptor from
             * this table (entry 5, the tess-factor ring), so without it the
             * merged LS/HS program has no ring to write its tessellation
             * factors into and the patch draw never retires at any
             * tessellation level. The compiler assigns the window-relative
             * dword; the hull's user-data window on gfx10 is
             * SPI_SHADER_USER_DATA_HS_0 at sh offset 0x10c, whose dword N is
             * SGPR 8+N for a merged program. */
            if(result.sh_count+2>PS5VK_DRAW_SH_CAPACITY)
                return VK_ERROR_UNKNOWN;
            result.sh[result.sh_count++]=(ps5_agc_register){
                (uint16_t)(0x10c+pair->tess_ring_table_slot),
                pair->tess_ring_table_low};
            result.sh[result.sh_count++]=(ps5_agc_register){
                (uint16_t)(0x10c+pair->tess_ring_table_slot+1u),
                pair->tess_ring_table_high};
#if defined(PS5VK_TESS_SYSTEM_TABLE) && PS5VK_TESS_SYSTEM_TABLE
            /* Diagnostic own-memory system table, not part of the compiler's
             * four-register hull package. Preserve its strict contract. */
            if(result.sh_count+2>PS5VK_DRAW_SH_CAPACITY)return VK_ERROR_UNKNOWN;
            result.sh[result.sh_count++]=(ps5_agc_register){0x102,pair->tess_ring_table_low};
            result.sh[result.sh_count++]=(ps5_agc_register){0x103,pair->tess_ring_table_high};
#endif
        }
        /* DIAGNOSTIC (PS5VK_TESS_LEGACY_DOMAIN): the legacy hardware-VS
         * domain's registers, after everything the NGG domain published
         * so the shared context registers take the legacy values. */
        if(pair->legacy_domain) {
            if(result.sh_count+pair->legacy_sh_count>PS5VK_DRAW_SH_CAPACITY ||
               result.cx_count+pair->legacy_cx_count>PS5VK_DRAW_CX_CAPACITY)
                return VK_ERROR_UNKNOWN;
            memcpy(result.sh+result.sh_count,pair->legacy_sh,
                pair->legacy_sh_count*sizeof(*result.sh));
            result.sh_count+=pair->legacy_sh_count;
            memcpy(result.cx+result.cx_count,pair->legacy_cx,
                pair->legacy_cx_count*sizeof(*result.cx));
            result.cx_count+=pair->legacy_cx_count;
        }
        result.runtime=pair->runtime_arguments;
        result.hull_runtime=pair->hull_arguments;
        /* This is the lab's audited draw-auto command modifier, not compiler
         * metadata. PSBC headers do not populate the legacy PAL field. */
        result.modifier=5;
    }
    if (!result.modifier) return VK_ERROR_UNKNOWN;
    /* Last fixed-state writes win over inherited/linked defaults. Explicitly
     * disable blending on subsequent non-blended draws, rather than retaining
     * the previous pipeline's state. Runtime acceptance is gated separately. */
    struct ps5vk_blend_words blend[PS5VK_MAX_COLOR_ATTACHMENTS];
    const VkBool32 dual_source=runtime && pair->dual_source_export?
        VK_TRUE:VK_FALSE;
    for(uint32_t attachment=0;attachment<color_count;++attachment)
        if(!ps5vk_blend_encode(&p->color_blend[attachment],p->blend_constants,
                               dual_source,&blend[attachment]))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    /* A DEPTH-ONLY draw programmes no colour target at all, so the blend word
     * pair written below is the one a colour pipeline with blending off emits:
     * explicit zeros and the same optimisation word. Writing it unconditionally
     * is what stops CB_BLEND0_CONTROL and SX_MRT0_BLEND_OPT from carrying the
     * previous draw's state into a pass that has no colour output. */
    if(!color_count) {
        const VkPipelineColorBlendAttachmentState none={0};
        if(!ps5vk_blend_encode(&none,p->blend_constants,dual_source,&blend[0]))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* Attachment zero's colour block came from ps5_pipeline_build; every other
     * attachment is appended as its own CB_COLORn block with its own blend con
     * control and optimisation, whose offsets are the next dwords. */
    if(result.cx_count+6u+
       (color_count>1u?(PS5VK_COLOR_TARGET_REGISTERS+2u)*(color_count-1u):0u)
       >PS5VK_DRAW_CX_CAPACITY)return VK_ERROR_UNKNOWN;
    for(uint32_t attachment=1;attachment<color_count;++attachment) {
        for(unsigned i=0;i<PS5VK_COLOR_TARGET_REGISTERS;++i)
            result.cx[result.cx_count++]=(ps5_agc_register){
                ps5vk_color_attachment_offsets[attachment][i],
                colors[attachment].registers[i].value};
        result.cx[result.cx_count++]=(ps5_agc_register){
            PS5VK_AGC_CB_BLEND_CONTROL(attachment),blend[attachment].control};
        result.cx[result.cx_count++]=(ps5_agc_register){
            PS5VK_AGC_SX_MRT_BLEND_OPT(attachment),blend[attachment].optimization};
    }
    result.cx[result.cx_count++]=(ps5_agc_register){0x1e0,blend[0].control};
    result.cx[result.cx_count++]=(ps5_agc_register){0x1d8,blend[0].optimization};
    for(unsigned i=0;i<4;++i)
        result.cx[result.cx_count++]=(ps5_agc_register){0x105+i,blend[0].constants[i]};
    if(runtime) {
        uint32_t spi_format=UINT32_MAX,shader_mask=UINT32_MAX;
        uint32_t per_target[PS5VK_MAX_COLOR_ATTACHMENTS][3],conversion[3];
        for(unsigned i=0;i<fs->header.num_cx_registers;++i) {
            if(fs->context[i].offset==0x1c5)spi_format=fs->context[i].value;
            if(fs->context[i].offset==0x08f)shader_mask=fs->context[i].value;
        }
        for(uint32_t attachment=0;attachment<color_count;++attachment)
            if(!ps5vk_color_export_state(p->color_format[attachment],spi_format,
                shader_mask,p->color_blend[attachment].blendEnable,dual_source,
                per_target[attachment]))
                return VK_ERROR_FEATURE_NOT_PRESENT;
        ps5vk_color_export_compose(per_target,color_count,conversion);
        if(result.cx_count+3u>PS5VK_DRAW_CX_CAPACITY)return VK_ERROR_UNKNOWN;
        /* Emit for unblended draws too: a previous FP16 blended draw must not
         * leave its downconversion active for the 32-bit export path. */
        for(unsigned i=0;i<3;++i)
            result.cx[result.cx_count++]=(ps5_agc_register){0x1d5+i,conversion[i]};
    }
#if defined(PS5VK_TESS_STATE_DUMP) && PS5VK_TESS_STATE_DUMP
    /* One patch draw and one ORDINARY runtime draw, so the two can be
     * diffed against each other.
     *
     * The tessellation pipeline's pre-raster stage is an NGG program and so
     * is the geometry probe's, and the geometry probe passes every case in
     * the same process on the same device through the same encoder. That
     * makes its emitted banks a real working control for every pre-raster
     * register the patch draw also programs - strictly more than a
     * host-compiled comparison, which can only see what the compiler
     * publishes and not the linked AGC block, the target state or anything
     * else the driver adds. Whatever the two share is not the defect;
     * whatever only the patch draw sets, or only the working draw sets, is
     * the entire remaining candidate list. */
    {
        static unsigned dumped_patch,dumped_plain;
        unsigned *once=has_tessellation?&dumped_patch:&dumped_plain;
        if(!*once && (has_tessellation || runtime)) {
            const char *tag=has_tessellation?"cx":"gcx";
            ++*once;
            tess_dump_bank(tag,result.cx,result.cx_count);
            tess_dump_bank(has_tessellation?"sh":"gsh",result.sh,result.sh_count);
            tess_dump_bank(has_tessellation?"uc":"guc",result.uc,result.uc_count);
        }
    }
#endif
    *out = result; return VK_SUCCESS;
}

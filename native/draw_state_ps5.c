#include "draw_state_ps5.h"
#include "viewport_ps5.h"
#include "blend_ps5.h"
#include "runtime_fragment_shape.h"
#include "sample_rate_diagnostic.h"
#include <string.h>

unsigned ps5vk_draw_state_site;
#define PS5VK_DRAW_UNSUPPORTED() do { \
    ps5vk_draw_state_site = __LINE__; \
    return VK_ERROR_FEATURE_NOT_PRESENT; \
} while (0)

#if PS5VK_SAMPLE_RATE_DIAGNOSTIC
/* The one definition of the diagnostic override, in the translation unit that
 * READS it: the probe only ever sets it through this declaration, and a build
 * that links the draw path without the probe archive still resolves. */
struct ps5vk_sample_rate_diagnostic_cx ps5vk_sample_rate_diagnostic_cx;
#endif
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
 * POLY_OFFSET_DB_IS_FLOAT_FMT for D32_SFLOAT, -16 fixed for D16_UNORM, and
 * zero when there is no depth attachment (Vulkan leaves the constant term's
 * unit undefined then). Both
 * front and back get the same scale/offset: Vulkan has one bias per polygon,
 * not per face. The factors are written even when the bias is disabled so the
 * words never carry another draw's values. */
static void polygon_offset(const struct ps5vk_raster_state *raster, VkFormat depth_format,
    ps5_agc_register out[6])
{
    const uint32_t slope = float_bits(raster->depth_bias_slope * 16.0f);
    const uint32_t offset = float_bits(raster->depth_bias_constant);
    /* PAL's GFX9+ depth view uses -16 fixed-point bits for Z_16 and -23
     * with the floating-format bit for Z_32_FLOAT. The D16 diagnostic only
     * accepts zero bias factors until nonzero bias has a native witness. */
    const uint32_t depth_bias_format = (depth_format == VK_FORMAT_D32_SFLOAT ||
                                        depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT) ?
        (((uint32_t)(-23) & 0xffu) | (1u << 8)) :
        depth_format == VK_FORMAT_D16_UNORM ? ((uint32_t)(-16) & 0xffu) : 0u;
    out[0] = (ps5_agc_register){0x2de, depth_bias_format};
    out[1] = (ps5_agc_register){0x2df, float_bits(raster->depth_bias_clamp)};
    out[2] = (ps5_agc_register){0x2e0, slope};
    out[3] = (ps5_agc_register){0x2e1, offset};
    out[4] = (ps5_agc_register){0x2e2, slope};
    out[5] = (ps5_agc_register){0x2e3, offset};
}
/* VkStencilOp to the GFX10 StencilOp code (Mesa RADV si_translate_stencil_op):
 * REPLACE writes the test value, the increments/decrements step by
 * STENCILOPVAL, which is therefore one. */
static int stencil_op(VkStencilOp op, uint32_t *out)
{
    static const uint32_t codes[8] = {
        0u, /* KEEP */ 1u, /* ZERO */ 3u, /* REPLACE -> REPLACE_TEST */
        5u, /* INCREMENT_AND_CLAMP -> ADD_CLAMP */ 6u, /* DECREMENT_AND_CLAMP -> SUB_CLAMP */
        7u, /* INVERT */ 8u, /* INCREMENT_AND_WRAP -> ADD_WRAP */
        9u, /* DECREMENT_AND_WRAP -> SUB_WRAP */
    };
    if ((unsigned)op >= 8u) return -1;
    *out = codes[op];
    return 0;
}

int ps5vk_stencil_registers(const VkStencilOpState *front, const VkStencilOpState *back,
    ps5_agc_register out[3])
{
    uint32_t ff, fp, fz, bf, bp, bz;
    if (!front || !back || !out || front->compareOp > VK_COMPARE_OP_ALWAYS ||
        back->compareOp > VK_COMPARE_OP_ALWAYS ||
        stencil_op(front->failOp, &ff) || stencil_op(front->passOp, &fp) ||
        stencil_op(front->depthFailOp, &fz) || stencil_op(back->failOp, &bf) ||
        stencil_op(back->passOp, &bp) || stencil_op(back->depthFailOp, &bz))
        return -1;
    /* DB_STENCIL_CONTROL: FAIL[3:0] ZPASS[7:4] ZFAIL[11:8], then the same
     * three for the back face at [15:12] [19:16] [23:20]. */
    out[0] = (ps5_agc_register){0x10b,
        ff | (fp << 4) | (fz << 8) | (bf << 12) | (bp << 16) | (bz << 20)};
    /* DB_STENCILREFMASK(_BF): TESTVAL[7:0] MASK[15:8] WRITEMASK[23:16]
     * OPVAL[31:24]; an 8-bit stencil aspect uses the low byte of each. */
    out[1] = (ps5_agc_register){0x10c, (front->reference & 0xffu) |
        ((front->compareMask & 0xffu) << 8) | ((front->writeMask & 0xffu) << 16) | (1u << 24)};
    out[2] = (ps5_agc_register){0x10d, (back->reference & 0xffu) |
        ((back->compareMask & 0xffu) << 8) | ((back->writeMask & 0xffu) << 16) | (1u << 24)};
    return 0;
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
    ps5vk_draw_state_site = 0;
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
             !ps5vk_color_target_format_supported(p->color_format[0])) :
            (color_count || !depth || p->color_format[0] != VK_FORMAT_UNDEFINED)) ||
        (p->cull_mode & ~VK_CULL_MODE_FRONT_AND_BACK) ||
        (p->front_face != VK_FRONT_FACE_CLOCKWISE && p->front_face != VK_FRONT_FACE_COUNTER_CLOCKWISE) ||
        p->depth_compare > VK_COMPARE_OP_ALWAYS || p->depth_compare < VK_COMPARE_OP_NEVER)
        PS5VK_DRAW_UNSUPPORTED();
    int depth_format_supported = p->depth_format == VK_FORMAT_D32_SFLOAT ||
        p->depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    /* The stencil test needs the stencil plane, which only the combined
     * target programmes. */
    if (raster->stencil_test && p->depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT)
        PS5VK_DRAW_UNSUPPORTED();
    depth_format_supported |= p->depth_format == VK_FORMAT_D16_UNORM &&
        width == 128u && height == 128u;
    if (p->depth_format == VK_FORMAT_UNDEFINED ? depth != NULL :
        (!depth_format_supported || !depth || depth->count != PS5_DEPTH_REGISTER_COUNT ||
         (p->depth_format == VK_FORMAT_D16_UNORM && raster->depth_bias_enable &&
          (raster->depth_bias_constant != 0.0f || raster->depth_bias_clamp != 0.0f ||
           raster->depth_bias_slope != 0.0f))))
        PS5VK_DRAW_UNSUPPORTED();
    struct ps5vk_native_graphics_pipeline *native = p->graphics_state;
    if (native->device != p->device || !native->pair || !native->pair->ready) return VK_ERROR_UNKNOWN;
    struct ps5vk_graphics_pair *pair = native->pair;
    int runtime=pair->runtime_arguments.enabled!=0;
    /* Which attachment each hardware colour target programmes. The pinned
     * render-pass module's second-target-only shape EXPORTS INTO MRT0 while the
     * pipeline writes only its second attachment: compiling a Location-0-only
     * and a Location-1-only module with the pinned PSBC produces identical
     * machine code (same exp target), and only the register pair differs
     * (CB_SHADER_MASK 0xf0 instead of 0xf). The export instruction therefore
     * carries no target, and the coherent programming is to renumber: the
     * attachment the pipeline really writes takes hardware target zero with its
     * own target block, blend control, write-mask nibble and conversion, and
     * the slot nothing writes stays masked. Every other shape programmes the
     * attachments positionally. */
    uint32_t slot[PS5VK_MAX_COLOR_ATTACHMENTS];
    for(unsigned i=0;i<PS5VK_MAX_COLOR_ATTACHMENTS;++i)slot[i]=(uint32_t)i;
    if(runtime && pair->fragment_shape==PS5VK_RUNTIME_FRAGMENT_SHAPE_SECOND_MRT) {
        slot[0]=1; slot[1]=0;
    }
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
    if (pair->vertex_quantization != 0x2d) PS5VK_DRAW_UNSUPPORTED();
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
    if (ps5_pipeline_build(&base, color_count ? colors[slot[0]].registers : NULL, &pair->cx, &pair->uc,
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
            target_mask |= (p->color_blend[slot[attachment]].colorWriteMask & 0xfu) << (4u * attachment);
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
    /* The stencil test (public GFX10 DB_DEPTH_CONTROL: STENCIL_ENABLE[0],
     * BACKFACE_ENABLE[7], STENCILFUNC[10:8], STENCILFUNC_BF[22:20]; the
     * hardware compare codes equal VkCompareOp). Front and back always carry
     * their own state, so BACKFACE_ENABLE is set whenever the test is. */
    if (depth && raster->stencil_test) {
        depth_control |= 1u | (1u << 7) |
            ((uint32_t)raster->stencil_front.compareOp << 8) |
            ((uint32_t)raster->stencil_back.compareOp << 20);
    }
    result.cx[result.cx_count++] = (ps5_agc_register){0x200, depth_control};
    if (depth && raster->stencil_test) {
        ps5_agc_register stencil[3];
        if (ps5vk_stencil_registers(&raster->stencil_front, &raster->stencil_back, stencil))
            PS5VK_DRAW_UNSUPPORTED();
        for (unsigned k = 0; k < 3; ++k) result.cx[result.cx_count++] = stencil[k];
    }
    /* Multisample raster state (DXVK262-T06). The colour target already states
     * its sample geometry in CB_COLOR0_ATTRIB; these are the raster half the
     * same draw needs, and they are written only when the pipeline carries more
     * than one sample so every single-sample draw emits exactly the words it
     * always did.
     *
     *   PA_SC_AA_CONFIG  (0x2f8) MSAA_NUM_SAMPLES[0:2] and
     *                            MSAA_EXPOSED_SAMPLES[20:22] = log2(count),
     *                            MAX_SAMPLE_DIST[13:16] = the largest offset
     *                            the sample pattern asks for (6/16 at 4x,
     *                            4/16 at 2x - PAL's ComputeMaxSampleDistance)
     *   DB_EQAA          (0x201) MAX_ANCHOR_SAMPLES[0:2],
     *                            PS_ITER_SAMPLES[4:6],
     *                            MASK_EXPORT_NUM_SAMPLES[8:10] and
     *                            ALPHA_TO_MASK_NUM_SAMPLES[12:14] =
     *                            log2 of the count, PS_ITER_SAMPLES being the
     *                            number of pixel iterations the shader runs
     *   PA_SC_MODE_CNTL_1 (0x293) PS_ITER_SAMPLE[16] when per-sample shading is
     *                            on, which forces the pixel wave to iterate per
     *                            sample instead of once per pixel
     *   PA_SC_MODE_CNTL_0 (0x292) MSAA_ENABLE[0] for a multisampled draw, set
     *                            where that word is written below
     *   PA_SC_AA_SAMPLE_LOCS_PIXEL_* (0x2fe..0x30d) the sample pattern itself
     *   SPI_BARYC_CNTL   (0x1b8) POS_FLOAT_LOCATION[16:17] = 2 - the float
     *                            position is computed AT THE ITERATED SAMPLE
     *                            NUMBER - whenever the wave iterates per
     *                            sample, and 0 (pixel centre) otherwise
     *
     * The last two are what the pinned min_sample_shading leaves need and what
     * this profile did not have. Measured on the witness that colours each
     * sample with fract(gl_FragCoord.xy): with the pattern and the location
     * set, the four samples of a 4x draw receive exactly (0.375,0.125),
     * (0.875,0.375), (0.125,0.625) and (0.625,0.875) - Vulkan's standard 4x
     * locations - and without them every sample receives the pixel centre,
     * (0.5,0.5), which is what made the oracle's unique-colour count
     * unreachable. The registers and their encoding come from the pinned PAL
     * source (gfx9MsaaState.cpp SetQuadSamplePattern and
     * ComputeMaxSampleDistance): sixteen context words, four samples each, X in
     * the low nibble of a byte and Y in the high one as a signed offset from
     * the pixel centre in 1/16 pixel units, all four pixels of the quad sharing
     * the same pattern. */
    const uint32_t draw_sample_count = ps5vk_sample_count_number(p->samples);
    {
        const uint32_t sample_count = draw_sample_count;
        if (sample_count > 1) {
            const uint32_t log_samples = ps5vk_sample_count_log2(p->samples);
            /* The fraction decides how many samples each fragment invocation
             * covers: minSampleShading 1.0 asks for one invocation per sample,
             * which is the shape the witness measures. */
            uint32_t iterations = sample_count;
            if (p->sample_shading_enable && p->min_sample_shading < 1.0f) {
                const float wanted = (float)sample_count * p->min_sample_shading;
                iterations = 1u;
                while ((float)iterations < wanted) iterations <<= 1u;
                if (iterations > sample_count) iterations = sample_count;
            }
            if (!p->sample_shading_enable) iterations = 0u;
            uint32_t iterations_log = 0u;
            while ((1u << iterations_log) < iterations) ++iterations_log;
            /* Vulkan's standard sample locations, as signed 1/16-pixel offsets
             * from the pixel centre, packed four samples to a word: X in the
             * low nibble of each byte and Y in the high one. */
            uint32_t location_word = 0u, max_sample_dist = 4u;
            if (sample_count == 4u) {
                /* (0.375,0.125) (0.875,0.375) (0.125,0.625) (0.625,0.875) */
                location_word = UINT32_C(0x622ae6ae);
                max_sample_dist = 6u;
            } else {
                /* (0.75,0.75) (0.25,0.25); the other two samples do not exist
                 * at this count and their nibbles stay zero. */
                location_word = UINT32_C(0x0000cc44);
            }
            const uint32_t aa_config = (log_samples & 0x7u) |
                ((max_sample_dist & 0xfu) << 13u) | ((log_samples & 0x7u) << 20u);
            const uint32_t db_eqaa = (log_samples & 0x7u) |
                ((iterations_log & 0x7u) << 4u) |
                ((log_samples & 0x7u) << 8u) |
                ((log_samples & 0x7u) << 12u);
            const uint32_t mode_cntl_1 =
                p->sample_shading_enable ? (UINT32_C(1) << 16u) : 0u;
            if (result.cx_count + 17u > PS5VK_DRAW_CX_CAPACITY) return VK_ERROR_UNKNOWN;
            result.cx[result.cx_count++] = (ps5_agc_register){0x2f8, aa_config};
            result.cx[result.cx_count++] = (ps5_agc_register){0x201, db_eqaa};
            result.cx[result.cx_count++] = (ps5_agc_register){0x293, mode_cntl_1};
            for (uint32_t i = 0; i < 16u; ++i)
                result.cx[result.cx_count++] =
                    (ps5_agc_register){(uint16_t)(0x2feu + i), location_word};
            /* The position's location follows the iteration, not the API flag:
             * a sample-shaded pipeline that asks for a fraction small enough to
             * keep one invocation per pixel must keep the pixel-centre
             * position, which is what the compiler's own coordinate shape was
             * chosen for (native/runtime_graphics_compiler.c publishes the
             * pipeline's sample-shading state to the standalone compile). */
            {
                const uint32_t baryc = iterations > 1u ?
                    (UINT32_C(2) << 16u) :
                    (UINT32_C(0) << 16u);
                unsigned replaced = 0;
                for (unsigned k = 0; k < result.cx_count; ++k)
                    if (result.cx[k].offset == 0x1b8u) { result.cx[k].value = baryc; ++replaced; }
                if (!replaced) {
                    if (result.cx_count + 1u > PS5VK_DRAW_CX_CAPACITY) return VK_ERROR_UNKNOWN;
                    result.cx[result.cx_count++] =
                        (ps5_agc_register){0x1b8u, baryc};
                }
            }
        }
    }
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
    else if (raster->polygon_mode != VK_POLYGON_MODE_FILL) PS5VK_DRAW_UNSUPPORTED();
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
    /* PA_SC_MODE_CNTL_0, matching RADV gfx10: VPORT_SCISSOR_ENABLE and
     * ALTERNATE_RBS_PER_TILE, no line stipple. MSAA_ENABLE is added for a
     * multisampled draw, exactly as PAL sets it whenever the stage's coverage
     * samples are more than one (gfx9MsaaState.cpp: "coverageSamples > 1").
     * Without it the rasteriser has no sample locations to work with, so the
     * pattern this draw programs at 0x2fe..0x30d never applies and every
     * sample of a pixel keeps the pixel centre - measured: a 4x draw whose
     * fragment colours each sample with fract(gl_FragCoord.xy) left one value
     * (0.5,0.5) in the target until this bit was set. Keep the generic scissor
     * at the target bounds and apply the Vulkan scissor/render-area
     * intersection to viewport zero. */
    result.cx[result.cx_count++] = (ps5_agc_register){0x292,
        UINT32_C(0x22) | (draw_sample_count > 1u ? UINT32_C(1) : UINT32_C(0))};
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
    polygon_offset(raster, depth ? p->depth_format : VK_FORMAT_UNDEFINED,
        result.cx + result.cx_count);
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
            PS5VK_DRAW_UNSUPPORTED();
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
        /* The second-target-only shape is programmed as ONE target, because its
         * export instruction writes MRT0 (measured: the Location-1-only module
         * compiles to the same code as the Location-0-only one, and only the
         * register pair differs). The compiler put the export's code in the
         * format's nibble zero and its enable in the mask's nibble ONE, because
         * it numbers the format list by declaration order and the mask by the
         * attachment each export targets; with the written attachment renumbered
         * to target zero, the driver takes that code and moves the enable down
         * beside it. Every other package is programmed exactly as the compiler
         * published it. */
        if(pair->fragment_shape==PS5VK_RUNTIME_FRAGMENT_SHAPE_SECOND_MRT)
            for(unsigned i=0;i<result.cx_count;++i) {
                if(result.cx[i].offset==0x1c5u)result.cx[i].value&=0xfu;
                else if(result.cx[i].offset==0x08fu)
                    result.cx[i].value=(result.cx[i].value>>4)&0xfu;
            }
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
        if(!ps5vk_blend_encode(&p->color_blend[slot[attachment]],p->blend_constants,
                               dual_source,&blend[attachment]))
            PS5VK_DRAW_UNSUPPORTED();
    /* A DEPTH-ONLY draw programmes no colour target at all, so the blend word
     * pair written below is the one a colour pipeline with blending off emits:
     * explicit zeros and the same optimisation word. Writing it unconditionally
     * is what stops CB_BLEND0_CONTROL and SX_MRT0_BLEND_OPT from carrying the
     * previous draw's state into a pass that has no colour output. */
    if(!color_count) {
        const VkPipelineColorBlendAttachmentState none={0};
        if(!ps5vk_blend_encode(&none,p->blend_constants,dual_source,&blend[0]))
            PS5VK_DRAW_UNSUPPORTED();
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
                colors[slot[attachment]].registers[i].value};
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
        /* Read the pair from the bank this draw programmes, after the shape's
         * own normalisation above: what is checked here is what the hardware
         * receives. */
        for(unsigned i=0;i<result.cx_count;++i) {
            if(result.cx[i].offset==0x1c5)spi_format=result.cx[i].value;
            if(result.cx[i].offset==0x08f)shader_mask=result.cx[i].value;
        }
        for(uint32_t attachment=0;attachment<color_count;++attachment) {
            if(pair->fragment_shape==PS5VK_RUNTIME_FRAGMENT_SHAPE_SECOND_MRT &&
               !p->color_write_mask[slot[attachment]]) {
                /* This shape's first target is the one whose export the
                 * compiler dropped: the pinned render-pass module's
                 * attachment_write_mask leaf writes only the second target. The
                 * normalised pair must therefore say nothing about the target
                 * nothing writes - that is what makes the tear a provable
                 * absence rather than a dropped value - and it converts
                 * nothing. Every other shape keeps its own contract, where a
                 * zero write mask simply programmes CB_TARGET_MASK. */
                if(((spi_format>>(4u*attachment))&0xfu) ||
                   ((shader_mask>>(4u*attachment))&0xfu))
                    PS5VK_DRAW_UNSUPPORTED();
                per_target[attachment][0]=0;
                per_target[attachment][1]=0;
                per_target[attachment][2]=0;
                continue;
            }
            if(!ps5vk_color_export_state(attachment,p->color_format[slot[attachment]],
                spi_format,shader_mask,p->color_blend[slot[attachment]].blendEnable,
                dual_source,per_target[attachment]))
                PS5VK_DRAW_UNSUPPORTED();
        }
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
#if PS5VK_SAMPLE_RATE_DIAGNOSTIC
    /* DIAGNOSTIC (DXVK262-T06 sample-rate line): publish the pixel-context
     * words the probe asked for instead of the ones this draw computed, so one
     * payload can ask the hardware what each state does.
     *
     * The override runs HERE, at the end, and it removes every earlier entry of
     * the same register before appending its own. Both halves matter, and the
     * first version of this survey got the second one wrong: it replaced
     * entries in place right after the compiled context block, and the driver's
     * own multisample words (PA_SC_MODE_CNTL_0, PA_SC_MODE_CNTL_1, DB_EQAA,
     * PA_SC_AA_CONFIG) are appended LATER in this function, so those overrides
     * were silently overwritten and their measurements said the register does
     * not matter when the register had never taken the requested value. A
     * stream that names a register twice leaves the last write in force, which
     * is what this makes explicit rather than accidental. Empty for every
     * ordinary draw. */
    for(uint32_t i=0;i<ps5vk_sample_rate_diagnostic_cx.count;++i) {
        const uint32_t index=ps5vk_sample_rate_diagnostic_cx.index[i];
        const uint32_t value=ps5vk_sample_rate_diagnostic_cx.value[i];
        unsigned k=0,kept=0;
        while(k<result.cx_count) {
            if(result.cx[k].offset==index) { k++; continue; }
            result.cx[kept++]=result.cx[k++];
        }
        result.cx_count=kept;
        if(result.cx_count+1>PS5VK_DRAW_CX_CAPACITY)return VK_ERROR_UNKNOWN;
        result.cx[result.cx_count++]=(ps5_agc_register){(uint16_t)index,value};
    }
#endif
    *out = result; return VK_SUCCESS;
}

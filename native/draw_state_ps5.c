#include "draw_state_ps5.h"
#include "viewport_ps5.h"
#include <string.h>
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
    const struct ps5vk_target_registers *color,
    const struct ps5vk_target_registers *depth, const VkRect2D *area,
    uint32_t width, uint32_t height, struct ps5vk_draw_state *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (!viewport_count || viewport_count > PS5VK_MAX_VIEWPORTS) return VK_ERROR_UNKNOWN;
    if (!p || !viewport_state || !scissor_state || !raster || !p->graphics || !p->graphics_state || !color || color->count != 16 ||
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
    const struct ps5vk_runtime_shader *vs=&pair->runtime_vertex,*fs=&pair->runtime_fragment;
    if(runtime && (vs->header.num_cx_registers>PS5VK_RUNTIME_CX_MAX ||
        fs->header.num_cx_registers>PS5VK_RUNTIME_CX_MAX ||
        !vs->header.num_sh_registers || !fs->header.num_sh_registers ||
        vs->header.num_sh_registers>PS5VK_RUNTIME_SH_MAX ||
        fs->header.num_sh_registers>PS5VK_RUNTIME_SH_MAX))return VK_ERROR_UNKNOWN;
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
    memcpy(result.sh, base.sh, sizeof(base.sh)); memcpy(result.uc, base.uc, sizeof(base.uc));
    result.modifier = pair->gs.specials.draw_modifier;
    if(runtime) {
        result.sh_count=vs->header.num_sh_registers+fs->header.num_sh_registers;
        memcpy(result.sh,vs->shader,vs->header.num_sh_registers*sizeof(*result.sh));
        memcpy(result.sh+vs->header.num_sh_registers,fs->shader,fs->header.num_sh_registers*sizeof(*result.sh));
        result.runtime=pair->runtime_arguments;
        /* This is the lab's audited draw-auto command modifier, not compiler
         * metadata. PSBC headers do not populate the legacy PAL field. */
        result.modifier=5;
    }
    if (!result.modifier) return VK_ERROR_UNKNOWN;
    *out = result; return VK_SUCCESS;
}

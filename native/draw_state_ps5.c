#include "draw_state_ps5.h"
#include "viewport_ps5.h"
#include <string.h>
VkResult ps5vk_native_draw_state(VkPipeline p, const struct ps5vk_target_registers *color,
    const struct ps5vk_target_registers *depth, const VkRect2D *area,
    uint32_t width, uint32_t height, struct ps5vk_draw_state *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (!p || !p->graphics || !p->graphics_state || !color || color->count != 16 ||
        !width || !height || width > 16384 || height > 16384 ||
        p->color_format != VK_FORMAT_B8G8R8A8_UNORM ||
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
    if (pair->vertex_quantization != 0x2d) return VK_ERROR_FEATURE_NOT_PRESENT;
    ps5_agc_register viewport[PS5VK_VIEWPORT_REGISTERS];
    VkResult rc = ps5vk_native_viewport(&p->viewport, &p->scissor, area, viewport);
    if (rc != VK_SUCCESS) return rc;
    struct ps5_pipeline_registers base;
    if (ps5_pipeline_build(&base, color->registers, &pair->cx, &pair->uc,
        pair->gs.cx, pair->ps.cx, pair->gs.sh, pair->ps.sh, width, height)) return VK_ERROR_UNKNOWN;
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
    if (depth) {
        memcpy(result.cx + result.cx_count, depth->registers, depth->count * sizeof(ps5_agc_register));
        result.cx_count += depth->count;
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
    memcpy(result.sh, base.sh, sizeof(base.sh)); memcpy(result.uc, base.uc, sizeof(base.uc));
    result.modifier = pair->gs.specials.draw_modifier;
    if (!result.modifier) return VK_ERROR_UNKNOWN;
    *out = result; return VK_SUCCESS;
}

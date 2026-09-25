#include "depth_stencil_layout.h"
#include "vk_pipeline.h"
#include "vk_render_pass.h"
#include "graphics_program.h"
#include "vk_pipeline_cache.h"
#include "color_attachment_contract.h"
#include "vk_transform_feedback.h"
#if (defined(PS5VK_TESS_PROBE) && PS5VK_TESS_PROBE) || (defined(PS5VK_GEOMETRY_KEY_DIAG) && PS5VK_GEOMETRY_KEY_DIAG)
#define PS5VK_PIPELINE_DIAGNOSTICS 1
#endif
/* Console-only markers: the host build has neither the transport nor the
 * header on its include path, and a pipeline that never gets created is
 * exactly what "the case died before it recorded anything" looks like. */
#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#define PIPE_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define PIPE_MARK(...) ((void)0)
#endif
#include <float.h>
#include <string.h>
/* Every plain feature refusal gets a site number, so a console run can name
 * the condition the way the adapter's rejection diagnostic does. The variable
 * lives here: the refusals are this file's, and the diagnostics read it. */
unsigned ps5vk_pipeline_refusal_site_value;
unsigned ps5vk_pipeline_refusal_site_value_state(void)
{
    return ps5vk_pipeline_refusal_site_value;
}
unsigned ps5vk_pipeline_refusal_site(void)
{
    return ps5vk_pipeline_refusal_site_value;
}
static VkResult refuse(unsigned site)
{
    ps5vk_pipeline_refusal_site_value=site;
#ifdef PS5VK_PIPELINE_DIAGNOSTICS
    ps5log_printf(PS5LOG_MARK,"PS5VK_PIPELINE_REFUSE site=%u",site);
#endif
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
static int finite_float(float value) { return value >= -FLT_MAX && value <= FLT_MAX; }
/* The multisample state this profile executes (DXVK262-T06).
 *
 * The counts come from the platform's own mask, so a build that never measured
 * a multisample path stays exactly as closed as it was: 1x only. The pipeline's
 * count must also be the one the subpass it draws in uses, because a mismatch
 * would render with a state no attachment has (VUID-VkGraphicsPipelineCreateInfo-
 * subpass-00757).
 *
 * Per-sample shading additionally needs sampleRateShading enabled on the
 * logical device: turning one fragment invocation into one per sample is the
 * capability that feature names, and a device whose application never enabled
 * it must not deliver the state anyway. minSampleShading is the fraction of
 * samples the implementation may shade at, so it is bounded to [0,1] while
 * sample shading is on and canonicalized to zero when it is off, where Vulkan
 * leaves it ignored.
 *
 * pSampleMask may only ask for what this path delivers: every sample of the
 * pipeline's count covered. An absent mask means exactly that (Vulkan defines
 * the absent mask as all bits set), and a mask may carry further bits - those
 * name samples this count does not have, so they are ignored. Masking off a
 * sample the target DOES have would need per-pixel mask registers, which
 * nothing on this path writes, so that shape is refused rather than silently
 * delivered as full coverage. */
static int multisample_state(VkDevice d, const VkPipelineMultisampleStateCreateInfo *m,
                             VkSampleCountFlagBits attachment_samples, int has_attachment)
{
    const VkSampleCountFlags supported =
        ps5vk_platform_sample_counts(d->platform_features);
    if (!(supported & m->rasterizationSamples)) return 0;
    if (has_attachment && m->rasterizationSamples != attachment_samples) return 0;
    if (m->sampleShadingEnable) {
        if (!(d->enabled_features & PS5VK_FEATURE_SAMPLE_RATE_SHADING)) return 0;
        if (!finite_float(m->minSampleShading) ||
            m->minSampleShading < 0.0f || m->minSampleShading > 1.0f) return 0;
    }
    const VkSampleMask used = ps5vk_sample_count_full_mask(m->rasterizationSamples);
    if (m->pSampleMask && (m->pSampleMask[0] & used) != used)
        return 0;
    return 1;
}
/* The rasterization state's pNext chain. This profile implements no optional
 * rasterization structure, and every state that would change behaviour is
 * refused, but the pinned upstream rasterization module chains one
 * VkPipelineRasterizationLineStateCreateInfoEXT unconditionally - it sets the
 * sType even when VK_EXT_line_rasterization is not enabled, which is the case
 * on this device - so refusing every pNext fails a module this profile
 * otherwise implements, at pipeline creation
 * (measured: dEQP-VK.rasterization.culling.* -> vk.createGraphicsPipelines
 * VK_ERROR_FEATURE_NOT_PRESENT in the 2026-09-18 acceptance run).
 *
 * The one structure is therefore accepted only in the exact form that asks for
 * what this driver already does: the default rectangular rasterization mode and
 * no stipple. Everything else stays refused: any other sType, a non-default
 * line rasterization mode, an enabled stipple, or a second structure in the
 * chain. */
static int rasterization_pnext_supported(const void *pnext)
{
    if(!pnext)return 1;
    const VkBaseInStructure *base_=pnext;
    if(base_->sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT)return 0;
    const VkPipelineRasterizationLineStateCreateInfoEXT *line=(const void *)base_;
    if(line->pNext)return 0;
    return line->lineRasterizationMode==VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT &&
        !line->stippledLineEnable;
}
/* The core dynamic states this profile executes: viewport, scissor and depth
 * bias. Every other VkDynamicState stays refused, so a pipeline can never be
 * created with a dynamic state that no draw would honour. */
/* The stencil states (compare mask, write mask, reference) are dynamic
 * values the draw folds into its stencil snapshot; they are honoured on the
 * combined depth/stencil attachment the stencil test runs on. */
/* VK_EXT_extended_dynamic_state adds the PS5VK_EDS_* states, accepted only on
 * a device that enabled the extendedDynamicState feature. Vulkan forbids
 * naming a state twice (VUID-VkPipelineDynamicStateCreateInfo-pDynamicStates-
 * 01442) and naming both VIEWPORT and VIEWPORT_WITH_COUNT, or both SCISSOR and
 * SCISSOR_WITH_COUNT (VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-04132,
 * -04133). This profile also requires the two *_WITH_COUNT states together:
 * the shape DXVK always declares, and the only one whose draw-time count
 * agreement it checks. The topology and binding-stride states stay refused
 * because the native program bakes both. */
static int dynamic_states(VkDevice d, const VkPipelineDynamicStateCreateInfo *info,
                          VkBool32 *viewport, VkBool32 *scissor, VkBool32 *depth_bias,
                          VkBool32 stencil[3], uint32_t *eds)
{
    *viewport=*scissor=*depth_bias=VK_FALSE;
    stencil[0]=stencil[1]=stencil[2]=VK_FALSE;
    *eds=0;
    if(!info)return 1;
    if(info->sType!=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO ||
       info->pNext || info->flags || info->dynamicStateCount>16 ||
       (info->dynamicStateCount && !info->pDynamicStates))return 0;
    for(uint32_t i=0;i<info->dynamicStateCount;++i) {
        VkBool32 *flag=NULL; uint32_t bit=0;
        switch(info->pDynamicStates[i]) {
        case VK_DYNAMIC_STATE_VIEWPORT: flag=viewport; break;
        case VK_DYNAMIC_STATE_SCISSOR: flag=scissor; break;
        case VK_DYNAMIC_STATE_DEPTH_BIAS: flag=depth_bias; break;
        case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: flag=&stencil[0]; break;
        case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: flag=&stencil[1]; break;
        case VK_DYNAMIC_STATE_STENCIL_REFERENCE: flag=&stencil[2]; break;
        case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT: bit=PS5VK_EDS_VIEWPORT_WITH_COUNT; break;
        case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT: bit=PS5VK_EDS_SCISSOR_WITH_COUNT; break;
        case VK_DYNAMIC_STATE_CULL_MODE_EXT: bit=PS5VK_EDS_CULL_MODE; break;
        case VK_DYNAMIC_STATE_FRONT_FACE_EXT: bit=PS5VK_EDS_FRONT_FACE; break;
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT: bit=PS5VK_EDS_DEPTH_TEST_ENABLE; break;
        case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT: bit=PS5VK_EDS_DEPTH_WRITE_ENABLE; break;
        case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT: bit=PS5VK_EDS_DEPTH_COMPARE_OP; break;
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT: bit=PS5VK_EDS_DEPTH_BOUNDS_TEST_ENABLE; break;
        case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT: bit=PS5VK_EDS_STENCIL_TEST_ENABLE; break;
        case VK_DYNAMIC_STATE_STENCIL_OP_EXT: bit=PS5VK_EDS_STENCIL_OP; break;
        default: return 0;
        }
        if(flag) {
            if(*flag)return 0;
            *flag=VK_TRUE;
        } else {
            if(!d->extended_dynamic_state_enabled || (*eds & bit))return 0;
            *eds|=bit;
        }
    }
    const uint32_t counts=PS5VK_EDS_VIEWPORT_WITH_COUNT|PS5VK_EDS_SCISSOR_WITH_COUNT;
    if((*viewport && (*eds & PS5VK_EDS_VIEWPORT_WITH_COUNT)) ||
       (*scissor && (*eds & PS5VK_EDS_SCISSOR_WITH_COUNT)) ||
       ((*eds & counts) && (*eds & counts)!=counts))return 0;
    return 1;
}

static int stencil_op_state_valid(const VkStencilOpState *s)
{
    return s->failOp <= VK_STENCIL_OP_DECREMENT_AND_WRAP &&
        s->passOp <= VK_STENCIL_OP_DECREMENT_AND_WRAP &&
        s->depthFailOp <= VK_STENCIL_OP_DECREMENT_AND_WRAP &&
        s->compareOp <= VK_COMPARE_OP_ALWAYS;
}

static int specialization_key(const VkSpecializationInfo *info,
                              struct ps5vk_graphics_module_key *out)
{
    if(!info)return 1;
    if(info->mapEntryCount>64 || (info->mapEntryCount && !info->pMapEntries) ||
       (info->dataSize && !info->pData))return 0;
    for(uint32_t i=0;i<info->mapEntryCount;++i) {
        const VkSpecializationMapEntry *entry=&info->pMapEntries[i];
        if(!entry->size || entry->size>8 || entry->offset>info->dataSize ||
           entry->size>info->dataSize-entry->offset)return 0;
        uint32_t at=out->specialization_count++;
        out->specializations[at].constant_id=entry->constantID;
        out->specializations[at].size=(uint32_t)entry->size;
        memcpy(out->specializations[at].data,(const uint8_t *)info->pData+entry->offset,entry->size);
    }
    for(uint32_t i=1;i<out->specialization_count;++i) {
        struct ps5vk_graphics_specialization value=out->specializations[i];uint32_t j=i;
        while(j && out->specializations[j-1].constant_id>value.constant_id) {
            out->specializations[j]=out->specializations[j-1];--j;
        }
        out->specializations[j]=value;
    }
    for(uint32_t i=1;i<out->specialization_count;++i)
        if(out->specializations[i-1].constant_id==out->specializations[i].constant_id)return 0;
    return 1;
}

/* The tessellation state's chain. VK_KHR_maintenance2 adds exactly one
 * structure to it, VkPipelineTessellationDomainOriginStateCreateInfo, accepted
 * once and only on a device that enabled the extension. UPPER_LEFT is the
 * origin the state has without the structure, so it changes nothing.
 * LOWER_LEFT would need the evaluation stage's domain coordinate flipped, which
 * the compiler adapter does not do; it is refused rather than silently drawn
 * with the upper-left origin. tessellationShader itself stays unadvertised, so
 * no shipping pipeline reaches this. Any other structure stays fail-closed. */
static VkResult tessellation_domain_origin(VkDevice d,
    const VkPipelineTessellationStateCreateInfo *t)
{
    const VkBaseInStructure *next=(const VkBaseInStructure *)t->pNext;
    if (!next) return VK_SUCCESS;
    if (next->sType != VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO ||
        next->pNext || !d->maintenance2_extension_enabled)
        return refuse(11);
    const VkTessellationDomainOrigin origin=
        ((const VkPipelineTessellationDomainOriginStateCreateInfo *)next)->domainOrigin;
    if (origin == VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT) return VK_SUCCESS;
    if (origin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT) return refuse(11);
    return VK_ERROR_UNKNOWN;
}
/* A shader stage's chain. The pinned DXVK 2.6.2 always chains the stage's
 * VkShaderModuleCreateInfo, even when it also created the module
 * (dxvk_shader.cpp:1150-1177: the stage structure is zeroed before its module
 * is tested). An inline module is VK_KHR_maintenance5 / graphics-pipeline-
 * library behaviour this device does not expose, so a stage without a module
 * stays refused; the redundant structure is ignored only when it carries
 * exactly the words of the module the stage names, so it can never select
 * different code. Anything else in the chain stays fail-closed. */
static int stage_next_supported(const VkPipelineShaderStageCreateInfo *s)
{
    if (!s->pNext) return 1;
    const VkShaderModuleCreateInfo *inline_module = s->pNext;
    return inline_module->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO &&
        !inline_module->pNext && !inline_module->flags && s->module &&
        inline_module->pCode && inline_module->codeSize == s->module->word_count * 4u &&
        !memcmp(inline_module->pCode, s->module->words, inline_module->codeSize);
}
/* VK_KHR_dynamic_rendering: the pass a pipeline created with renderPass =
 * VK_NULL_HANDLE describes, built from its VkPipelineRenderingCreateInfo the
 * way vkCmdBeginRenderingKHR builds an instance's pass - colour attachments
 * in order, then the depth/stencil one - so the rest of creation, and every
 * draw, compares formats exactly as for a render-pass pipeline. The storage is
 * the caller's; nothing is retained. A hole in the colour list (an UNDEFINED
 * format), a view mask, or depth and stencil formats that differ are shapes
 * this profile does not build and stay refused. */
struct rendering_pass {
    struct VkRenderPass_T pass;
    VkAttachmentDescription attachments[PS5VK_MAX_COLOR_ATTACHMENTS + 1];
    struct ps5vk_subpass subpass;
};
static VkResult rendering_pass(VkDevice d, const VkGraphicsPipelineCreateInfo *in,
    struct rendering_pass *out)
{
    const VkPipelineRenderingCreateInfo *r = in->pNext;
    if (!d->dynamic_rendering_enabled || !r ||
        r->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) return refuse(2);
    if (r->pNext || r->viewMask ||
        r->colorAttachmentCount > PS5VK_MAX_COLOR_ATTACHMENTS ||
        (r->colorAttachmentCount && !r->pColorAttachmentFormats) || in->subpass ||
        !in->pMultisampleState)
        return refuse(2);
    const VkFormat depth = r->depthAttachmentFormat != VK_FORMAT_UNDEFINED ?
        r->depthAttachmentFormat : r->stencilAttachmentFormat;
    if (r->depthAttachmentFormat != VK_FORMAT_UNDEFINED &&
        r->stencilAttachmentFormat != VK_FORMAT_UNDEFINED &&
        r->depthAttachmentFormat != r->stencilAttachmentFormat) return refuse(2);
    memset(out, 0, sizeof(*out));
    const VkSampleCountFlagBits samples = in->pMultisampleState->rasterizationSamples;
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->colorAttachmentCount; ++i) {
        if (r->pColorAttachmentFormats[i] == VK_FORMAT_UNDEFINED) return refuse(2);
        out->attachments[n] = (VkAttachmentDescription){.format = r->pColorAttachmentFormats[i],
            .samples = samples, .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        out->subpass.color[n] = (VkAttachmentReference){n, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        ++n;
    }
    out->subpass.color_count = n;
    out->subpass.depth = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    if (depth != VK_FORMAT_UNDEFINED) {
        out->attachments[n] = (VkAttachmentDescription){.format = depth, .samples = samples,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        out->subpass.depth = (VkAttachmentReference){n,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        ++n;
    }
    out->pass.device = d;
    out->pass.attachment_count = n;
    out->pass.subpass_count = 1;
    out->pass.attachments = out->attachments;
    out->pass.subpasses = &out->subpass;
    return VK_SUCCESS;
}
static VkResult create(VkDevice d, const VkGraphicsPipelineCreateInfo *in,
                       const VkAllocationCallbacks *allocator, VkPipeline *out)
{
    if (in->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO || !in->layout ||
        in->layout->device != d || (in->renderPass && in->renderPass->device != d))
        return VK_ERROR_UNKNOWN;
    struct rendering_pass rendering;
    VkRenderPass render_pass = in->renderPass;
    if (!render_pass) {
        VkResult rc = rendering_pass(d, in, &rendering);
        if (rc != VK_SUCCESS) return rc;
        render_pass = &rendering.pass;
    }
    /* The pipeline is created for ONE subpass, which must exist in the pass it
     * names. A nonzero index is no longer refused outright: it identifies the
     * scope this pipeline may draw in. */
    /* Two stages are the vertex+fragment profile every earlier tranche used;
     * three through five add the optional tessellation control/evaluation pair
     * and geometry after evaluation. Nothing else is accepted, so a
     * mesh or task stage still fails here. */
    if ((in->pNext && in->renderPass) || in->flags || in->subpass >= render_pass->subpass_count ||
        (in->stageCount < 2 || in->stageCount > 5) || !in->pStages ||
        in->layout->set_count>PS5VK_MAX_SETS)
        return refuse(2);
    VkBool32 dynamic_viewport,dynamic_scissor,dynamic_depth_bias,dynamic_stencil[3];
    uint32_t eds;
    if(!dynamic_states(d,in->pDynamicState,&dynamic_viewport,&dynamic_scissor,&dynamic_depth_bias,
                       dynamic_stencil,&eds))
        return refuse(3);
    /* With *_WITH_COUNT the viewport and scissor arrays, and their count,
     * are the command buffer's: the static counts must be zero
     * (VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-03379, -03380). */
    const VkBool32 with_count=(eds & PS5VK_EDS_VIEWPORT_WITH_COUNT) ? VK_TRUE : VK_FALSE;
    if(with_count) dynamic_viewport=dynamic_scissor=VK_TRUE;
    const VkPipelineShaderStageCreateInfo *vs=NULL, *fs=NULL, *gs=NULL,
        *tcs=NULL, *tes=NULL;
    for (unsigned i=0; i<in->stageCount; ++i) {
        const VkPipelineShaderStageCreateInfo *s=&in->pStages[i];
        if (s->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || !s->module ||
            s->module->device != d || !s->pName) return VK_ERROR_UNKNOWN;
        if (s->flags || !stage_next_supported(s)) return refuse(4);
        uint32_t id;
        if (!ps5vk_shader_entry(s->module, s->stage, s->pName, &id)) return VK_ERROR_UNKNOWN;
        if (s->stage == VK_SHADER_STAGE_VERTEX_BIT && !vs) vs=s;
        else if (s->stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT && !tcs) tcs=s;
        else if (s->stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT && !tes) tes=s;
        else if (s->stage == VK_SHADER_STAGE_GEOMETRY_BIT && !gs) gs=s;
        else if (s->stage == VK_SHADER_STAGE_FRAGMENT_BIT && !fs) fs=s;
        else return refuse(5);
    }
    /* Vulkan requires the control and evaluation stages to appear together, and
     * the stage count to name exactly the stages that were provided. */
    if (!vs || !fs || (!!tcs != !!tes) ||
        in->stageCount != (unsigned)(2 + (tcs?2:0) + (gs?1:0)))
        return VK_ERROR_UNKNOWN;
    /* A geometry pipeline needs the feature the logical device enabled. The
     * private witness build keeps its own gate, exactly as the multiview
     * diagnostic does, so shipping behaviour stays the negotiation. The
     * tessellation pair is the same shape: its stages are the feature. */
#if !PS5VK_OPTIONAL_STAGE_DIAGNOSTIC
    if (gs && !(d->enabled_features & PS5VK_FEATURE_GEOMETRY_SHADER))
        return refuse(6);
    if (tcs && !(d->enabled_features & PS5VK_FEATURE_TESSELLATION_SHADER))
        return refuse(7);
#endif
    /* Transform feedback (VK_EXT_transform_feedback) is not negotiated on this
     * device, so no stage may declare the Xfb execution mode. Such a module
     * would otherwise be compiled and only refused later, by the native header
     * builder, when it sees streamout metadata; this site names the reason
     * before any compiler or backend work. A malformed capture interface is
     * refused here as well. */
    {
        static const struct ps5vk_xfb_limits no_capture;
        const VkPipelineShaderStageCreateInfo *const stages[]={vs,tcs,tes,gs,fs};
        for (unsigned i=0; i<sizeof(stages)/sizeof(stages[0]); ++i) {
            const VkPipelineShaderStageCreateInfo *s=stages[i];
            uint32_t id;
            struct ps5vk_xfb_interface xfb;
            if (!s) continue;
            if (!ps5vk_shader_entry(s->module, s->stage, s->pName, &id) ||
                ps5vk_xfb_reflect(s->module->words, s->module->word_count, id,
                                  &no_capture, &xfb) != PS5VK_XFB_NONE)
                return refuse(19);
        }
    }
    const VkPipelineVertexInputStateCreateInfo *v=in->pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *ia=in->pInputAssemblyState;
    const VkPipelineRasterizationStateCreateInfo *r=in->pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo *m=in->pMultisampleState;
    const VkPipelineViewportStateCreateInfo *vp=in->pViewportState;
    const VkPipelineColorBlendStateCreateInfo *b=in->pColorBlendState;
    /* The colour blend state is optional for the same reason the
     * depth-stencil state is: a pipeline for a DEPTH-ONLY subpass has no
     * colour attachment to blend into, and the pinned upstream depth clamp
     * module leaves pColorBlendState NULL there. The subpass decides which
     * of the two must be present; that check is below, where the subpass is
     * resolved. */
    if (!v || !ia || !r || !m || !vp) return VK_ERROR_UNKNOWN;
    if (v->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO ||
        ia->sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
        r->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
        m->sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
        vp->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO ||
        (b && b->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO)) return VK_ERROR_UNKNOWN;
    if (v->vertexBindingDescriptionCount>16 || v->vertexAttributeDescriptionCount>32 ||
        (v->vertexBindingDescriptionCount && !v->pVertexBindingDescriptions) ||
        (v->vertexAttributeDescriptionCount && !v->pVertexAttributeDescriptions))return refuse(8);
    for(uint32_t i=0;i<v->vertexBindingDescriptionCount;++i) {
        const VkVertexInputBindingDescription *binding=&v->pVertexBindingDescriptions[i];
        if(binding->binding>=16 || binding->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX ||
           !binding->stride || binding->stride>0x3fff)return refuse(9);
        for(uint32_t j=0;j<i;++j)
            if(v->pVertexBindingDescriptions[j].binding==binding->binding)return VK_ERROR_UNKNOWN;
    }
    for(uint32_t a=0;a<v->vertexAttributeDescriptionCount;++a)
        if(v->pVertexAttributeDescriptions[a].location>=32)return refuse(10);
    /* Tessellation contract. Vulkan requires the control and evaluation stages
     * together, PATCH_LIST as the input assembly when they are present, and a
     * patchControlPoints in range; pTessellationState is ignored without them.
     * The profile validates all of that and hands the pair to the compiler
     * adapter, which compiles the hull and domain programs; the runtime loader
     * refuses to package the hull while the launch state it needs is unwritten,
     * so a pipeline that reaches the hardware still cannot be a tessellation
     * one until that state exists. tessellationShader stays unadvertised. */
    if (tcs) {
        const VkPipelineTessellationStateCreateInfo *t=in->pTessellationState;
        if (!t || t->sType != VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO ||
            t->flags || !t->patchControlPoints ||
            t->patchControlPoints > PS5VK_MAX_PATCH_CONTROL_POINTS ||
            ia->topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
            return refuse(11);
        VkResult origin=tessellation_domain_origin(d,t);
        if (origin!=VK_SUCCESS) return origin;
    }
    if (in->pTessellationState &&
       in->pTessellationState->sType != VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO)
        return VK_ERROR_UNKNOWN;
    /* The topology decides the primitive the AGC linker programs, so the
     * accepted set and its values live in one place. */
    uint32_t primitive_type=0;
    if(ps5vk_agc_primitive_type(ia->topology,&primitive_type))return refuse(12);
    /* Points and lines are the input families a geometry stage is fed with, and
     * that is the only shape the profile has a witness for; without the stage
     * they stay refused. */
    if (!gs && ps5vk_agc_primitive_needs_geometry(primitive_type))
        return refuse(13);
    /* Primitive restart is input-assembly state: the front end compares each
     * index against a reset index and starts a new primitive where it matches,
     * so it can only act on a strip. The profile accepts it for the two strips
     * it carries and keeps refusing it everywhere else, where a restart index
     * could not do what the caller declared. The draw path programs the cut from
     * the pipeline's flag and the draw's index width. */
    if (ia->primitiveRestartEnable &&
        ia->topology != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP &&
        ia->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
        return refuse(14);
    /* The multisample state is judged against the subpass this pipeline draws
     * in: its colour attachment names the sample count the pipeline has to
     * agree with. The COUNT decides whether there is one - a depth-only subpass
     * declares none, and its reference array is not read - and a subpass with
     * no colour attachment leaves the count bounded only by the platform's own
     * mask. */
    const struct ps5vk_subpass *multisample_subpass =
        ps5vk_render_pass_subpass(render_pass, in->subpass);
    const uint32_t multisample_attachment = multisample_subpass->color[0].attachment;
    const int multisample_has_attachment = multisample_subpass->color_count &&
        multisample_attachment != VK_ATTACHMENT_UNUSED;
    if (v->pNext || v->flags ||
        /* primitiveRestartEnable is accepted above for the two strip
         * topologies it can act on, and refused there for every other
         * topology, so it is not part of this blanket refusal. */
        ia->pNext || ia->flags ||
        !rasterization_pnext_supported(r->pNext) || r->flags || r->rasterizerDiscardEnable ||
        /* depthClampEnable needs depthClamp ENABLED on this logical device;
         * the state itself executes (native PA_CL_CLIP_CNTL ZCLIP_*_DISABLE
         * with the viewport depth range as the clamp interval). */
        (r->depthClampEnable && !(d->enabled_features & PS5VK_FEATURE_DEPTH_CLAMP)) ||
        /* Depth bias executes: the constant and slope factors are programmed
         * as the polygon offset the draw runs with. Vulkan ignores all three
         * factors while depthBiasEnable is false and while the state is
         * dynamic, so they are checked only in the static enabled form, and
         * the only check is the feature rule: a non-zero clamp needs
         * depthBiasClamp ENABLED on this logical device. No finiteness rule
         * is invented; the factors keep their float bit patterns. */
        (r->depthBiasEnable && !dynamic_depth_bias && r->depthBiasClamp != 0.0f &&
         !(d->enabled_features & PS5VK_FEATURE_DEPTH_BIAS_CLAMP)) ||
        /* LINE and POINT polygon modes need fillModeNonSolid ENABLED; every
         * other value (FILL_RECTANGLE_NV) stays refused. wideLines is not
         * advertised, so the static line width is exactly 1.0. */
        (r->polygonMode != VK_POLYGON_MODE_FILL &&
         ((r->polygonMode != VK_POLYGON_MODE_LINE && r->polygonMode != VK_POLYGON_MODE_POINT) ||
          !(d->enabled_features & PS5VK_FEATURE_FILL_MODE_NON_SOLID))) ||
        r->lineWidth != 1.0f ||
        m->pNext || m->flags || m->alphaToCoverageEnable || m->alphaToOneEnable ||
        !multisample_state(d, m, multisample_has_attachment ?
                render_pass->attachments[multisample_attachment].samples :
                VK_SAMPLE_COUNT_1_BIT, multisample_has_attachment) ||
        /* Viewport arrays: the two counts must match and lie in
         * 1..PS5VK_MAX_VIEWPORTS; more than one needs multiViewport ENABLED
         * on this logical device. The count is static in this profile (no
         * *_WITH_COUNT dynamic state), so a zero count is malformed. */
        vp->pNext || vp->flags ||
        (with_count ? (vp->viewportCount || vp->scissorCount) : !vp->viewportCount) ||
        vp->viewportCount > PS5VK_MAX_VIEWPORTS ||
        vp->scissorCount != vp->viewportCount ||
        (vp->viewportCount > 1 && !(d->enabled_features & PS5VK_FEATURE_MULTI_VIEWPORT)) ||
        (b && !ps5vk_color_blend_state_shape_supported(b)))
        return refuse(15);
    /* Vulkan makes pColorBlendState optional. A subpass that names no colour
     * attachment may omit it, and then it describes exactly zero attachments,
     * which is the same count a supplied empty state carries. */
    const uint32_t blend_attachment_count = b ? b->attachmentCount : 0u;
    if ((!dynamic_viewport && !vp->pViewports) || (!dynamic_scissor && !vp->pScissors) ||
        (b && b->attachmentCount && !b->pAttachments)) return VK_ERROR_UNKNOWN;
    /* Every static element is validated before any is stored. */
    for (uint32_t i = 0; i < vp->viewportCount; ++i) {
        const VkViewport *viewport=&vp->pViewports[i]; const VkRect2D *scissor=&vp->pScissors[i];
        if ((!dynamic_viewport && (!finite_float(viewport->x) || !finite_float(viewport->y) ||
            !finite_float(viewport->width) || !finite_float(viewport->height) ||
            !(viewport->width > 0) ||
            /* VK_KHR_maintenance1 permits a negative height. */
            !(viewport->height > 0 ||
              (d->maintenance1_extension_enabled && viewport->height < 0)) ||
            !(viewport->minDepth >= 0 && viewport->minDepth <= 1) ||
            !(viewport->maxDepth >= 0 && viewport->maxDepth <= 1))) ||
            (!dynamic_scissor && (scissor->offset.x < 0 || scissor->offset.y < 0 ||
            !scissor->extent.width || !scissor->extent.height)))
            return VK_ERROR_UNKNOWN;
    }
    /* A state declared dynamic makes its static value ignored, so only the
     * static ones are validated. */
    if ((!(eds & PS5VK_EDS_CULL_MODE) && (r->cullMode & ~VK_CULL_MODE_FRONT_AND_BACK)) ||
        (!(eds & PS5VK_EDS_FRONT_FACE) && r->frontFace != VK_FRONT_FACE_CLOCKWISE &&
         r->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE))
        return VK_ERROR_UNKNOWN;
    const VkPipelineDepthStencilStateCreateInfo *depth=in->pDepthStencilState;
    VkRenderPass pass=render_pass;
    /* The formats come from the subpass this pipeline names, not from the
     * first one: the identity is what a draw is later checked against. */
    const struct ps5vk_subpass *subpass = ps5vk_render_pass_subpass(pass, in->subpass);
    if (subpass->depth.attachment != VK_ATTACHMENT_UNUSED && !depth) return VK_ERROR_UNKNOWN;
    /* One blend attachment per colour attachment the subpass names, and none
     * for the DEPTH-ONLY shape. The exact agreement is checked below, where the
     * per-attachment state is read out of the subpass and the pipeline. */
    /* The stencil test runs only on an attachment that has a stencil aspect:
     * the combined D32_SFLOAT_S8_UINT surface. A depth-only attachment keeps
     * refusing it rather than silently dropping the test. */
    const VkFormat depth_attachment_format = subpass->depth.attachment == VK_ATTACHMENT_UNUSED ?
        VK_FORMAT_UNDEFINED : pass->attachments[subpass->depth.attachment].format;
    const int stencil_aspect =
        (ps5vk_format_aspects(depth_attachment_format) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    /* A dynamic depth-bounds, stencil-test or compare-op state makes the
     * static value ignored; the draw enforces the dynamic one. A dynamic
     * stencil test may be enabled at draw time, so the static ops it would
     * then use are validated whenever that is possible. */
    const int stencil_possible = depth &&
        (depth->stencilTestEnable || (eds & PS5VK_EDS_STENCIL_TEST_ENABLE));
    if (depth && (depth->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
        depth->pNext || depth->flags ||
        (depth->depthBoundsTestEnable && !(eds & PS5VK_EDS_DEPTH_BOUNDS_TEST_ENABLE)) ||
        (depth->stencilTestEnable && !(eds & PS5VK_EDS_STENCIL_TEST_ENABLE) && !stencil_aspect) ||
        (stencil_possible && !(eds & PS5VK_EDS_STENCIL_OP) &&
            (!stencil_op_state_valid(&depth->front) || !stencil_op_state_valid(&depth->back))) ||
        (!(eds & PS5VK_EDS_DEPTH_COMPARE_OP) &&
         (depth->depthCompareOp < VK_COMPARE_OP_NEVER ||
          depth->depthCompareOp > VK_COMPARE_OP_ALWAYS))))
        return refuse(16);
    struct ps5vk_graphics_key key={
        .vertex={.words=vs->module->words,.word_count=vs->module->word_count,.entry=vs->pName},
        .fragment={.words=fs->module->words,.word_count=fs->module->word_count,.entry=fs->pName},
        .geometry=gs? (struct ps5vk_graphics_module_key){
            .words=gs->module->words,.word_count=gs->module->word_count,.entry=gs->pName} :
            (struct ps5vk_graphics_module_key){0},
        .tess_control=tcs? (struct ps5vk_graphics_module_key){
            .words=tcs->module->words,.word_count=tcs->module->word_count,.entry=tcs->pName} :
            (struct ps5vk_graphics_module_key){0},
        .tess_eval=tes? (struct ps5vk_graphics_module_key){
            .words=tes->module->words,.word_count=tes->module->word_count,.entry=tes->pName} :
            (struct ps5vk_graphics_module_key){0},
        .patch_control_points=tcs?in->pTessellationState->patchControlPoints:0,
        .feature_mask=d->enabled_features,
        .topology=ia->topology,
        .samples=m->rasterizationSamples,
        /* Sample shading is carried exactly as the accepted state reads it:
         * the flag, the fraction (zero while the flag is off, where Vulkan
         * ignores it) and the mask. Zero is the canonical "every implemented
         * sample is covered" state, which is what an absent pSampleMask means
         * and what every mask shape this contract accepts also means, so a
         * pipeline that spells its mask out shares one program with a pipeline
         * that leaves it absent. */
        .sample_shading_enable=m->sampleShadingEnable?VK_TRUE:VK_FALSE,
        .min_sample_shading=m->sampleShadingEnable?m->minSampleShading:0.0f,
        .sample_mask=0u,
        .vertex_binding_count=v->vertexBindingDescriptionCount,.vertex_attribute_count=v->vertexAttributeDescriptionCount,
        .vertex_bindings=v->pVertexBindingDescriptions,.vertex_attributes=v->pVertexAttributeDescriptions,
        .descriptor_set_count=in->layout->set_count,.descriptor_sets=in->layout->sets,
        .push_constant_size=in->layout->push_constant_size};
    memcpy(key.push_constant_stages,in->layout->push_constant_stages,
           sizeof(key.push_constant_stages));
    /* One element per colour attachment the subpass names: the format comes
     * from the pass, the blend state from the pipeline. Vulkan requires the
     * pipeline to describe exactly as many attachments as the subpass. A
     * subpass that names none is the DEPTH-ONLY shape: the count stays zero and
     * every colour field keeps the value the zeroed initialiser gave it
     * (VK_FORMAT_UNDEFINED, no write mask, no blend), which is exactly what the
     * compiler and the native path key the depth-only case on. */
    if (blend_attachment_count != subpass->color_count) return refuse(15);
    key.color_attachment_count = subpass->color_count;
    int any_blend = 0;
    /* Vulkan's independentBlend is what makes element i of pAttachments the
     * state of attachment i. Without it the elements past the first are not
     * independent: the first one describes every attachment, and programming
     * whatever the application left in the others would render with state the
     * specification says does not apply. The write mask is different - it is
     * per attachment either way - so only the blend fields are folded. */
    const int independent_blend = (d->enabled_features & PS5VK_FEATURE_INDEPENDENT_BLEND) != 0;
    for (uint32_t attachment = 0; attachment < subpass->color_count; ++attachment) {
        const VkPipelineColorBlendAttachmentState *a =
            &b->pAttachments[independent_blend ? attachment : 0];
        key.color_format[attachment] =
            pass->attachments[subpass->color[attachment].attachment].format;
        /* An integer colour target is not blended into and its lanes are not
         * converted, so a blend state on it describes a shape this profile
         * cannot program. */
        if(ps5vk_color_target_format_is_integer(key.color_format[attachment]) &&
           b->pAttachments[attachment].blendEnable) return refuse(15);
        key.color_write_mask[attachment] = b->pAttachments[attachment].colorWriteMask;
        key.blend_enable[attachment] = a->blendEnable;
        if (!a->blendEnable) continue;
        any_blend = 1;
        key.src_color_blend_factor[attachment] = a->srcColorBlendFactor;
        key.dst_color_blend_factor[attachment] = a->dstColorBlendFactor;
        key.color_blend_op[attachment] = a->colorBlendOp;
        key.src_alpha_blend_factor[attachment] = a->srcAlphaBlendFactor;
        key.dst_alpha_blend_factor[attachment] = a->dstAlphaBlendFactor;
        key.alpha_blend_op[attachment] = a->alphaBlendOp;
    }
    if (any_blend)
        memcpy(key.blend_constants, b->blendConstants, sizeof(key.blend_constants));
    /* SRC1 names the fragment shader's secondary output for attachment zero.
     * The effective state is the key's - already folded to attachment zero's
     * element when independentBlend is not enabled - so a SRC1 equation the
     * application left in an element the specification ignores does not refuse
     * the pipeline. The compiler separately proves that the selected fragment
     * module really exports the secondary value; the two checks prevent either
     * state alone from authorizing a draw. */
    if(!(d->enabled_features & PS5VK_FEATURE_DUAL_SRC_BLEND)) {
        for (uint32_t attachment = 0; attachment < key.color_attachment_count; ++attachment)
            if (ps5vk_color_attachment_uses_src1(
                    &b->pAttachments[independent_blend ? attachment : 0]))
                return refuse(18);
    }
    if(!specialization_key(vs->pSpecializationInfo,&key.vertex) ||
       !specialization_key(fs->pSpecializationInfo,&key.fragment) ||
       (gs && !specialization_key(gs->pSpecializationInfo,&key.geometry)) ||
       (tcs && !specialization_key(tcs->pSpecializationInfo,&key.tess_control)) ||
       (tes && !specialization_key(tes->pSpecializationInfo,&key.tess_eval)))
        return refuse(17);
    const void *data=NULL;
    const struct ps5vk_graphics_program *program=NULL;
    VkResult rc;
    if(d->graphics_acquire) {
        rc=d->graphics_acquire(d->graphics_compiler_context,&key,&data);
#ifdef PS5VK_PIPELINE_DIAGNOSTICS
        if(rc!=VK_SUCCESS)
            ps5log_printf(PS5LOG_MARK,"PS5VK_PIPELINE_ACQUIRE_FAIL rc=%d",(int)rc);
#endif
        if(rc!=VK_SUCCESS || !data) {
            if(data)d->graphics_compiled_release(d->graphics_compiler_context,data);
            return rc==VK_SUCCESS?VK_ERROR_INITIALIZATION_FAILED:rc;
        }
    } else {
        rc=ps5vk_graphics_resolve(d->graphics_library,&key,&program);
        if(rc!=VK_SUCCESS)return rc;
        data=program->backend_data;
    }
    VkAllocationCallbacks saved={0}; VkBool32 custom=VK_FALSE;
    VkPipeline p=ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL,allocator,
        sizeof(*p),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,&saved,&custom);
    if (!p) {
        if(d->graphics_acquire)d->graphics_compiled_release(d->graphics_compiler_context,data);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(p,0,sizeof(*p));
    rc=d->graphics_create(d,data,primitive_type,&p->graphics_state);
#ifdef PS5VK_PIPELINE_DIAGNOSTICS
    if(rc!=VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK,"PS5VK_PIPELINE_NATIVE_CREATE_FAIL rc=%d",(int)rc);
#endif
    if(d->graphics_acquire)d->graphics_compiled_release(d->graphics_compiler_context,data);
    if(rc==VK_SUCCESS && p->graphics_state && d->graphics_used_sets) {
        rc=d->graphics_used_sets(d,p->graphics_state,&p->graphics_used_set_mask);
        if(rc==VK_SUCCESS &&
           (p->graphics_used_set_mask & ~((1u<<in->layout->set_count)-1u)))
            rc=VK_ERROR_INITIALIZATION_FAILED;
        if(rc==VK_SUCCESS)p->graphics_usage_known=VK_TRUE;
    }
    if (rc != VK_SUCCESS || !p->graphics_state) {
        if (p->graphics_state) d->graphics_release(d,p->graphics_state);
        ps5vk_object_free(p,&saved,custom);
        return rc == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : rc;
    }
    p->device=d; p->allocator=saved; p->custom_allocator=custom; p->graphics=VK_TRUE;
    p->subpass=in->subpass;
    p->dynamic_rendering=in->renderPass?VK_FALSE:VK_TRUE;
    /* The multisample state the native draw state reads (DXVK262-T06): the
     * count the accepted state carries, and the shading flag and fraction the
     * loader turns into pixel iterations. */
    p->samples=key.samples;
    p->sample_shading_enable=key.sample_shading_enable;
    p->min_sample_shading=key.min_sample_shading;
    p->set_count=in->layout->set_count;
    if(p->set_count)memcpy(p->sets,in->layout->sets,p->set_count*sizeof(*p->sets));
    p->graphics_release=d->graphics_release;
    p->dynamic_viewport=dynamic_viewport;p->dynamic_scissor=dynamic_scissor;
    p->viewport_count=vp->viewportCount;
    if(!dynamic_viewport)memcpy(p->viewports,vp->pViewports,vp->viewportCount*sizeof(*p->viewports));
    if(!dynamic_scissor)memcpy(p->scissors,vp->pScissors,vp->viewportCount*sizeof(*p->scissors));
    /* The enable is always static in this profile; the factors are static
     * only when VK_DYNAMIC_STATE_DEPTH_BIAS was not declared, and a disabled
     * bias keeps zero factors so a snapshot never carries ignored values. */
    p->dynamic_depth_bias=dynamic_depth_bias;
    p->raster.depth_clamp=r->depthClampEnable?VK_TRUE:VK_FALSE;
    p->raster.polygon_mode=r->polygonMode;
    p->raster.depth_bias_enable=r->depthBiasEnable?VK_TRUE:VK_FALSE;
    if(r->depthBiasEnable && !dynamic_depth_bias) {
        p->raster.depth_bias_constant=r->depthBiasConstantFactor;
        p->raster.depth_bias_clamp=r->depthBiasClamp;
        p->raster.depth_bias_slope=r->depthBiasSlopeFactor;
    }
    p->push_constant_size=in->layout->push_constant_size;
    memcpy(p->push_constant_stages,in->layout->push_constant_stages,
           sizeof(p->push_constant_stages));
    p->cull_mode=r->cullMode; p->front_face=r->frontFace;
    p->color_attachment_count=key.color_attachment_count;
    for(uint32_t attachment=0;attachment<key.color_attachment_count;++attachment) {
        p->color_format[attachment]=key.color_format[attachment];
        p->color_write_mask[attachment]=key.color_write_mask[attachment];
    }
    p->primitive_restart=ia->primitiveRestartEnable;
    /* The pipeline carries one blend state per colour attachment it was
     * created for; with none (the depth-only shape) the array keeps the zeroed
     * value the object allocation gave it. */
    if(p->color_attachment_count)
        memcpy(p->color_blend,b->pAttachments,
               p->color_attachment_count*sizeof(*p->color_blend));
    memcpy(p->blend_constants,key.blend_constants,sizeof(p->blend_constants));
    p->vertex_binding_count=key.vertex_binding_count;p->vertex_attribute_count=key.vertex_attribute_count;
    if(key.vertex_binding_count)memcpy(p->vertex_bindings,key.vertex_bindings,
        key.vertex_binding_count*sizeof(*key.vertex_bindings));
    if(key.vertex_attribute_count)memcpy(p->vertex_attributes,key.vertex_attributes,
        key.vertex_attribute_count*sizeof(*key.vertex_attributes));
    if (subpass->depth.attachment != VK_ATTACHMENT_UNUSED) {
        p->depth_format=pass->attachments[subpass->depth.attachment].format;
        p->depth_test=depth->depthTestEnable; p->depth_write=depth->depthWriteEnable;
        p->depth_compare=depth->depthCompareOp;
        /* The stencil state is kept whole; the draw replaces the masks and
         * the reference the pipeline declared dynamic. */
        if(depth->stencilTestEnable || (eds & PS5VK_EDS_STENCIL_TEST_ENABLE)) {
            p->raster.stencil_test=depth->stencilTestEnable && stencil_aspect;
            p->raster.stencil_front=depth->front;
            p->raster.stencil_back=depth->back;
        }
    }
    /* A dynamic state's static value is ignored by Vulkan and never reaches a
     * draw (the recorder resolves it from the command buffer), so the object
     * keeps a canonical valid value rather than whatever the caller left. */
    p->dynamic_eds=eds;
    if(eds & PS5VK_EDS_CULL_MODE)p->cull_mode=VK_CULL_MODE_NONE;
    if(eds & PS5VK_EDS_FRONT_FACE)p->front_face=VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if(eds & PS5VK_EDS_DEPTH_TEST_ENABLE)p->depth_test=VK_FALSE;
    if(eds & PS5VK_EDS_DEPTH_WRITE_ENABLE)p->depth_write=VK_FALSE;
    if(eds & PS5VK_EDS_DEPTH_COMPARE_OP)p->depth_compare=VK_COMPARE_OP_NEVER;
    p->dynamic_stencil_compare_mask=dynamic_stencil[0];
    p->dynamic_stencil_write_mask=dynamic_stencil[1];
    p->dynamic_stencil_reference=dynamic_stencil[2];
    ++d->pipeline_objects;
    *out=p;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache,
    uint32_t count, const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    for (uint32_t i=0;i<count;++i) out[i]=VK_NULL_HANDLE;
    if (!d || !count || !infos) return VK_ERROR_UNKNOWN;
    for (uint32_t i=0;i<count;++i)
        PIPE_MARK("PS5VK_PIPELINE_CREATE subpass=%u samples=%u stages=%u topology=%u vb=%u va=%u "
            "colors=%u dyn=%u ds=%u rp=%u",
            (unsigned)infos[i].subpass,
            (unsigned)(infos[i].pMultisampleState ?
                infos[i].pMultisampleState->rasterizationSamples : 0u),
            (unsigned)infos[i].stageCount,
            (unsigned)(infos[i].pInputAssemblyState ?
                infos[i].pInputAssemblyState->topology : 0xffffffffu),
            (unsigned)(infos[i].pVertexInputState ?
                infos[i].pVertexInputState->vertexBindingDescriptionCount : 0u),
            (unsigned)(infos[i].pVertexInputState ?
                infos[i].pVertexInputState->vertexAttributeDescriptionCount : 0u),
            (unsigned)(infos[i].pColorBlendState ? infos[i].pColorBlendState->attachmentCount : 0u),
            (unsigned)(infos[i].pDynamicState != NULL),
            (unsigned)(infos[i].pDepthStencilState != NULL),
            (unsigned)(infos[i].renderPass != NULL));
    /* A live same-device cache is accepted and carries no portable records yet. */
    if ((cache && !ps5vk_pipeline_cache_usable(d, cache)) ||
        !d->graphics_enabled || (!d->graphics_library && !d->graphics_acquire) ||
        (!!d->graphics_acquire != !!d->graphics_compiled_release) ||
        !d->graphics_create || !d->graphics_release)
        return refuse(18);
    VkResult rc=VK_SUCCESS;
    for (uint32_t i=0;i<count;++i) {
        VkResult current=create(d,&infos[i],allocator,&out[i]);
        if (rc == VK_SUCCESS && current != VK_SUCCESS) rc=current;
    }
    return rc;
}

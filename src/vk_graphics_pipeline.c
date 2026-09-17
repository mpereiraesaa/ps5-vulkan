#include "vk_pipeline.h"
#include "vk_render_pass.h"
#include "graphics_program.h"
#include "vk_pipeline_cache.h"
#include <float.h>
#include <string.h>
static int finite_float(float value) { return value >= -FLT_MAX && value <= FLT_MAX; }
/* The core dynamic states this profile executes: viewport, scissor and depth
 * bias. Every other VkDynamicState stays refused, so a pipeline can never be
 * created with a dynamic state that no draw would honour. */
static int dynamic_states(const VkPipelineDynamicStateCreateInfo *info,
                          VkBool32 *viewport, VkBool32 *scissor, VkBool32 *depth_bias)
{
    *viewport=*scissor=*depth_bias=VK_FALSE;
    if(!info)return 1;
    if(info->sType!=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO ||
       info->pNext || info->flags || info->dynamicStateCount>3 ||
       (info->dynamicStateCount && !info->pDynamicStates))return 0;
    for(uint32_t i=0;i<info->dynamicStateCount;++i) {
        VkBool32 *flag;
        if(info->pDynamicStates[i]==VK_DYNAMIC_STATE_VIEWPORT)flag=viewport;
        else if(info->pDynamicStates[i]==VK_DYNAMIC_STATE_SCISSOR)flag=scissor;
        else if(info->pDynamicStates[i]==VK_DYNAMIC_STATE_DEPTH_BIAS)flag=depth_bias;
        else return 0;
        if(*flag)return 0;
        *flag=VK_TRUE;
    }
    return 1;
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

static VkResult create(VkDevice d, const VkGraphicsPipelineCreateInfo *in,
                       const VkAllocationCallbacks *allocator, VkPipeline *out)
{
    if (in->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO || !in->layout ||
        in->layout->device != d || !in->renderPass || in->renderPass->device != d) return VK_ERROR_UNKNOWN;
    /* The pipeline is created for ONE subpass, which must exist in the pass it
     * names. A nonzero index is no longer refused outright: it identifies the
     * scope this pipeline may draw in. */
    if (in->pNext || in->flags || in->subpass >= in->renderPass->subpass_count ||
        in->stageCount != 2 || !in->pStages ||
        in->layout->set_count>PS5VK_MAX_SETS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkBool32 dynamic_viewport,dynamic_scissor,dynamic_depth_bias;
    if(!dynamic_states(in->pDynamicState,&dynamic_viewport,&dynamic_scissor,&dynamic_depth_bias))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkPipelineShaderStageCreateInfo *vs=NULL, *fs=NULL;
    for (unsigned i=0; i<2; ++i) {
        const VkPipelineShaderStageCreateInfo *s=&in->pStages[i];
        if (s->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || !s->module ||
            s->module->device != d || !s->pName) return VK_ERROR_UNKNOWN;
        if (s->flags || s->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
        uint32_t id;
        if (!ps5vk_shader_entry(s->module, s->stage, s->pName, &id)) return VK_ERROR_UNKNOWN;
        if (s->stage == VK_SHADER_STAGE_VERTEX_BIT && !vs) vs=s;
        else if (s->stage == VK_SHADER_STAGE_FRAGMENT_BIT && !fs) fs=s;
        else return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (!vs || !fs) return VK_ERROR_UNKNOWN;
    const VkPipelineVertexInputStateCreateInfo *v=in->pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *ia=in->pInputAssemblyState;
    const VkPipelineRasterizationStateCreateInfo *r=in->pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo *m=in->pMultisampleState;
    const VkPipelineViewportStateCreateInfo *vp=in->pViewportState;
    const VkPipelineColorBlendStateCreateInfo *b=in->pColorBlendState;
    if (!v || !ia || !r || !m || !vp || !b) return VK_ERROR_UNKNOWN;
    if (v->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO ||
        ia->sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
        r->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
        m->sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
        vp->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO ||
        b->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO) return VK_ERROR_UNKNOWN;
    if (v->vertexBindingDescriptionCount>16 || v->vertexAttributeDescriptionCount>32 ||
        (v->vertexBindingDescriptionCount && !v->pVertexBindingDescriptions) ||
        (v->vertexAttributeDescriptionCount && !v->pVertexAttributeDescriptions))return VK_ERROR_FEATURE_NOT_PRESENT;
    for(uint32_t i=0;i<v->vertexBindingDescriptionCount;++i) {
        const VkVertexInputBindingDescription *binding=&v->pVertexBindingDescriptions[i];
        if(binding->binding>=16 || binding->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX ||
           !binding->stride || binding->stride>0x3fff)return VK_ERROR_FEATURE_NOT_PRESENT;
        for(uint32_t j=0;j<i;++j)
            if(v->pVertexBindingDescriptions[j].binding==binding->binding)return VK_ERROR_UNKNOWN;
    }
    for(uint32_t a=0;a<v->vertexAttributeDescriptionCount;++a)
        if(v->pVertexAttributeDescriptions[a].location>=32)return VK_ERROR_FEATURE_NOT_PRESENT;
    /* The topology decides the primitive the AGC linker programs, so the
     * accepted set and its values live in one place. */
    uint32_t primitive_type=0;
    if(ps5vk_agc_primitive_type(ia->topology,&primitive_type))return VK_ERROR_FEATURE_NOT_PRESENT;
    if (v->pNext || v->flags ||
        ia->pNext || ia->flags || ia->primitiveRestartEnable ||
        r->pNext || r->flags || r->depthClampEnable || r->rasterizerDiscardEnable ||
        /* Depth bias executes: the constant and slope factors are programmed
         * as the polygon offset the draw runs with. Vulkan ignores all three
         * factors while depthBiasEnable is false and while the state is
         * dynamic, so they are checked only in the static enabled form, and
         * the only check is the feature rule: a non-zero clamp needs
         * depthBiasClamp ENABLED on this logical device. No finiteness rule
         * is invented; the factors keep their float bit patterns. */
        (r->depthBiasEnable && !dynamic_depth_bias && r->depthBiasClamp != 0.0f &&
         !(d->enabled_features & PS5VK_FEATURE_DEPTH_BIAS_CLAMP)) ||
        r->polygonMode != VK_POLYGON_MODE_FILL || r->lineWidth != 1.0f ||
        m->pNext || m->flags || m->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
        m->sampleShadingEnable || m->alphaToCoverageEnable || m->alphaToOneEnable ||
        (m->pSampleMask && !(m->pSampleMask[0] & 1)) ||
        vp->pNext || vp->flags || vp->viewportCount != 1 || vp->scissorCount != 1 ||
        b->pNext || b->flags || b->logicOpEnable || b->attachmentCount != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((!dynamic_viewport && !vp->pViewports) || (!dynamic_scissor && !vp->pScissors) ||
        !b->pAttachments) return VK_ERROR_UNKNOWN;
    const VkViewport *viewport=vp->pViewports; const VkRect2D *scissor=vp->pScissors;
    if ((!dynamic_viewport && (!finite_float(viewport->x) || !finite_float(viewport->y) ||
        !finite_float(viewport->width) || !finite_float(viewport->height) ||
        !(viewport->width > 0) || !(viewport->height > 0) ||
        !(viewport->minDepth >= 0 && viewport->minDepth <= 1) ||
        !(viewport->maxDepth >= 0 && viewport->maxDepth <= 1))) ||
        (!dynamic_scissor && (scissor->offset.x < 0 || scissor->offset.y < 0 ||
        !scissor->extent.width || !scissor->extent.height)) ||
        r->cullMode & ~VK_CULL_MODE_FRONT_AND_BACK ||
        (r->frontFace != VK_FRONT_FACE_CLOCKWISE && r->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE))
        return VK_ERROR_UNKNOWN;
    const VkPipelineDepthStencilStateCreateInfo *depth=in->pDepthStencilState;
    VkRenderPass pass=in->renderPass;
    /* The formats come from the subpass this pipeline names, not from the
     * first one: the identity is what a draw is later checked against. */
    const struct ps5vk_subpass *subpass = ps5vk_render_pass_subpass(pass, in->subpass);
    if (subpass->depth.attachment != VK_ATTACHMENT_UNUSED && !depth) return VK_ERROR_UNKNOWN;
    if (depth && (depth->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
        depth->pNext || depth->flags || depth->depthBoundsTestEnable || depth->stencilTestEnable ||
        depth->depthCompareOp < VK_COMPARE_OP_NEVER || depth->depthCompareOp > VK_COMPARE_OP_ALWAYS))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_graphics_key key={
        .vertex={.words=vs->module->words,.word_count=vs->module->word_count,.entry=vs->pName},
        .fragment={.words=fs->module->words,.word_count=fs->module->word_count,.entry=fs->pName},
        .topology=ia->topology, .color_format=pass->attachments[subpass->color.attachment].format,
        .samples=m->rasterizationSamples, .color_write_mask=b->pAttachments[0].colorWriteMask,
        .blend_enable=b->pAttachments[0].blendEnable,
        .vertex_binding_count=v->vertexBindingDescriptionCount,.vertex_attribute_count=v->vertexAttributeDescriptionCount,
        .vertex_bindings=v->pVertexBindingDescriptions,.vertex_attributes=v->pVertexAttributeDescriptions,
        .descriptor_set_count=in->layout->set_count,.descriptor_sets=in->layout->sets,
        .push_constant_size=in->layout->push_constant_size};
    memcpy(key.push_constant_stages,in->layout->push_constant_stages,
           sizeof(key.push_constant_stages));
    if(!specialization_key(vs->pSpecializationInfo,&key.vertex) ||
       !specialization_key(fs->pSpecializationInfo,&key.fragment))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const void *data=NULL;
    const struct ps5vk_graphics_program *program=NULL;
    VkResult rc;
    if(d->graphics_acquire) {
        rc=d->graphics_acquire(d->graphics_compiler_context,&key,&data);
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
    if(d->graphics_acquire)d->graphics_compiled_release(d->graphics_compiler_context,data);
    if (rc != VK_SUCCESS || !p->graphics_state) {
        if (p->graphics_state) d->graphics_release(d,p->graphics_state);
        ps5vk_object_free(p,&saved,custom);
        return rc == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : rc;
    }
    p->device=d; p->allocator=saved; p->custom_allocator=custom; p->graphics=VK_TRUE;
    p->subpass=in->subpass;
    p->set_count=in->layout->set_count;
    if(p->set_count)memcpy(p->sets,in->layout->sets,p->set_count*sizeof(*p->sets));
    p->graphics_release=d->graphics_release;
    p->dynamic_viewport=dynamic_viewport;p->dynamic_scissor=dynamic_scissor;
    if(!dynamic_viewport)p->viewport=*viewport;
    if(!dynamic_scissor)p->scissor=*scissor;
    /* The enable is always static in this profile; the factors are static
     * only when VK_DYNAMIC_STATE_DEPTH_BIAS was not declared, and a disabled
     * bias keeps zero factors so a snapshot never carries ignored values. */
    p->dynamic_depth_bias=dynamic_depth_bias;
    p->raster.depth_bias_enable=r->depthBiasEnable?VK_TRUE:VK_FALSE;
    if(r->depthBiasEnable && !dynamic_depth_bias) {
        p->raster.depth_bias_constant=r->depthBiasConstantFactor;
        p->raster.depth_bias_clamp=r->depthBiasClamp;
        p->raster.depth_bias_slope=r->depthBiasSlopeFactor;
    }
    p->push_constant_size=in->layout->push_constant_size;
    memcpy(p->push_constant_stages,in->layout->push_constant_stages,
           sizeof(p->push_constant_stages));
    p->cull_mode=r->cullMode; p->front_face=r->frontFace; p->color_format=key.color_format;
    p->vertex_binding_count=key.vertex_binding_count;p->vertex_attribute_count=key.vertex_attribute_count;
    if(key.vertex_binding_count)memcpy(p->vertex_bindings,key.vertex_bindings,
        key.vertex_binding_count*sizeof(*key.vertex_bindings));
    if(key.vertex_attribute_count)memcpy(p->vertex_attributes,key.vertex_attributes,
        key.vertex_attribute_count*sizeof(*key.vertex_attributes));
    if (subpass->depth.attachment != VK_ATTACHMENT_UNUSED) {
        p->depth_format=pass->attachments[subpass->depth.attachment].format;
        p->depth_test=depth->depthTestEnable; p->depth_write=depth->depthWriteEnable;
        p->depth_compare=depth->depthCompareOp;
    }
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
    /* A live same-device cache is accepted and carries no portable records yet. */
    if ((cache && !ps5vk_pipeline_cache_usable(d, cache)) ||
        !d->graphics_enabled || (!d->graphics_library && !d->graphics_acquire) ||
        (!!d->graphics_acquire != !!d->graphics_compiled_release) ||
        !d->graphics_create || !d->graphics_release)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult rc=VK_SUCCESS;
    for (uint32_t i=0;i<count;++i) {
        VkResult current=create(d,&infos[i],allocator,&out[i]);
        if (rc == VK_SUCCESS && current != VK_SUCCESS) rc=current;
    }
    return rc;
}

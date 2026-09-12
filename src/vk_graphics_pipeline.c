#include "vk_pipeline.h"
#include "vk_render_pass.h"
#include "graphics_program.h"
#include <float.h>
#include <string.h>
static int finite_float(float value) { return value >= -FLT_MAX && value <= FLT_MAX; }

static VkResult create(VkDevice d, const VkGraphicsPipelineCreateInfo *in,
                       const VkAllocationCallbacks *allocator, VkPipeline *out)
{
    if (in->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO || !in->layout ||
        in->layout->device != d || !in->renderPass || in->renderPass->device != d) return VK_ERROR_UNKNOWN;
    if (in->pNext || in->flags || in->subpass || in->stageCount != 2 || !in->pStages ||
        in->pTessellationState || in->pDynamicState || in->layout->set_count>1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkPipelineShaderStageCreateInfo *vs=NULL, *fs=NULL;
    for (unsigned i=0; i<2; ++i) {
        const VkPipelineShaderStageCreateInfo *s=&in->pStages[i];
        if (s->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || !s->module ||
            s->module->device != d || !s->pName) return VK_ERROR_UNKNOWN;
        if (s->flags || s->pNext || s->pSpecializationInfo) return VK_ERROR_FEATURE_NOT_PRESENT;
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
    if (v->vertexBindingDescriptionCount>1 || v->vertexAttributeDescriptionCount>32 ||
        (v->vertexBindingDescriptionCount && (!v->pVertexBindingDescriptions ||
         v->pVertexBindingDescriptions[0].binding ||
         v->pVertexBindingDescriptions[0].inputRate!=VK_VERTEX_INPUT_RATE_VERTEX)) ||
        (v->vertexAttributeDescriptionCount && !v->pVertexAttributeDescriptions))return VK_ERROR_FEATURE_NOT_PRESENT;
    for(uint32_t a=0;a<v->vertexAttributeDescriptionCount;++a)
        if(v->pVertexAttributeDescriptions[a].location>=32)return VK_ERROR_FEATURE_NOT_PRESENT;
    if (v->pNext || v->flags ||
        ia->pNext || ia->flags || ia->primitiveRestartEnable || ia->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
        r->pNext || r->flags || r->depthClampEnable || r->rasterizerDiscardEnable || r->depthBiasEnable ||
        r->polygonMode != VK_POLYGON_MODE_FILL || r->lineWidth != 1.0f ||
        m->pNext || m->flags || m->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
        m->sampleShadingEnable || m->alphaToCoverageEnable || m->alphaToOneEnable ||
        (m->pSampleMask && !(m->pSampleMask[0] & 1)) ||
        vp->pNext || vp->flags || vp->viewportCount != 1 || vp->scissorCount != 1 ||
        b->pNext || b->flags || b->logicOpEnable || b->attachmentCount != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!vp->pViewports || !vp->pScissors || !b->pAttachments) return VK_ERROR_UNKNOWN;
    const VkViewport *viewport=vp->pViewports; const VkRect2D *scissor=vp->pScissors;
    if (!finite_float(viewport->x) || !finite_float(viewport->y) || !finite_float(viewport->width) ||
        !finite_float(viewport->height) || !(viewport->width > 0) || !(viewport->height > 0) ||
        !(viewport->minDepth >= 0 && viewport->minDepth <= 1) ||
        !(viewport->maxDepth >= 0 && viewport->maxDepth <= 1) ||
        scissor->offset.x < 0 || scissor->offset.y < 0 || !scissor->extent.width || !scissor->extent.height ||
        r->cullMode & ~VK_CULL_MODE_FRONT_AND_BACK ||
        (r->frontFace != VK_FRONT_FACE_CLOCKWISE && r->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE))
        return VK_ERROR_UNKNOWN;
    const VkPipelineDepthStencilStateCreateInfo *depth=in->pDepthStencilState;
    VkRenderPass pass=in->renderPass;
    if (pass->depth.attachment != VK_ATTACHMENT_UNUSED && !depth) return VK_ERROR_UNKNOWN;
    if (depth && (depth->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
        depth->pNext || depth->flags || depth->depthBoundsTestEnable || depth->stencilTestEnable ||
        depth->depthCompareOp < VK_COMPARE_OP_NEVER || depth->depthCompareOp > VK_COMPARE_OP_ALWAYS))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_graphics_key key={
        .vertex={vs->module->words,vs->module->word_count,vs->pName},
        .fragment={fs->module->words,fs->module->word_count,fs->pName},
        .topology=ia->topology, .color_format=pass->attachments[pass->color.attachment].format,
        .samples=m->rasterizationSamples, .color_write_mask=b->pAttachments[0].colorWriteMask,
        .blend_enable=b->pAttachments[0].blendEnable,
        .vertex_binding_count=v->vertexBindingDescriptionCount,.vertex_attribute_count=v->vertexAttributeDescriptionCount,
        .vertex_bindings=v->pVertexBindingDescriptions,.vertex_attributes=v->pVertexAttributeDescriptions,
        .descriptor_set_count=in->layout->set_count,.descriptor_sets=in->layout->sets};
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
    rc=d->graphics_create(d,data,&p->graphics_state);
    if(d->graphics_acquire)d->graphics_compiled_release(d->graphics_compiler_context,data);
    if (rc != VK_SUCCESS || !p->graphics_state) {
        if (p->graphics_state) d->graphics_release(d,p->graphics_state);
        ps5vk_object_free(p,&saved,custom);
        return rc == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : rc;
    }
    p->device=d; p->allocator=saved; p->custom_allocator=custom; p->graphics=VK_TRUE;
    p->set_count=in->layout->set_count;
    if(p->set_count)memcpy(p->sets,in->layout->sets,p->set_count*sizeof(*p->sets));
    p->graphics_release=d->graphics_release; p->viewport=*viewport; p->scissor=*scissor;
    p->cull_mode=r->cullMode; p->front_face=r->frontFace; p->color_format=key.color_format;
    p->vertex_binding_count=key.vertex_binding_count;p->vertex_attribute_count=key.vertex_attribute_count;
    if(key.vertex_binding_count)p->vertex_binding=*key.vertex_bindings;
    if(key.vertex_attribute_count)memcpy(p->vertex_attributes,key.vertex_attributes,
        key.vertex_attribute_count*sizeof(*key.vertex_attributes));
    if (pass->depth.attachment != VK_ATTACHMENT_UNUSED) {
        p->depth_format=pass->attachments[pass->depth.attachment].format;
        p->depth_test=depth->depthTestEnable; p->depth_write=depth->depthWriteEnable;
        p->depth_compare=depth->depthCompareOp;
    }
    ++d->pipeline_objects; *out=p; return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache,
    uint32_t count, const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    for (uint32_t i=0;i<count;++i) out[i]=VK_NULL_HANDLE;
    if (!d || !count || !infos) return VK_ERROR_UNKNOWN;
    if (cache || !d->graphics_enabled || (!d->graphics_library && !d->graphics_acquire) ||
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

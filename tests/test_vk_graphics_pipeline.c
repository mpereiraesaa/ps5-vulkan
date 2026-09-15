#include "vk_pipeline.h"
#include "vk_render_pass.h"
#include "graphics_program.h"
#include <assert.h>
#include <stdlib.h>
static unsigned created, released;
static unsigned acquired, compiled_released, compile_fail, backend_fail;
static VkResult backend(VkDevice d,const void *data,void **out)
{ (void)d; assert(data); ++created; *out=malloc(1); return backend_fail?VK_ERROR_UNKNOWN:(*out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY); }
static void release(VkDevice d,void *data) { (void)d; ++released; free(data); }
static VkResult acquire(void *context,const struct ps5vk_graphics_key *key,const void **out)
{
    assert(context==&acquired && key->vertex.word_count==10 && key->fragment.word_count==10);
    ++acquired;*out=malloc(1);assert(*out);
    return compile_fail?VK_ERROR_FEATURE_NOT_PRESENT:VK_SUCCESS;
}
static void compiled_release(void *context,const void *data)
{ assert(context==&acquired && data);++compiled_released;free((void *)data); }
int main(void)
{
    uint32_t vs[]={0x07230203,0x10000,0,2,0,(5u<<16)|15,0,1,0x6e69616d,0};
    uint32_t fs[]={0x07230203,0x10000,0,2,0,(5u<<16)|15,4,1,0x6e69616d,0};
    struct ps5vk_graphics_program program={.key={.vertex={.words=vs,.word_count=10,.entry="main"},.fragment={.words=fs,.word_count=10,.entry="main"},
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.color_format=VK_FORMAT_B8G8R8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask=15},.backend_data=vs};
    struct ps5vk_graphics_library library={&program,1};
    struct VkDevice_T d={.graphics_enabled=1,.graphics_library=&library,.graphics_create=backend,.graphics_release=release};
    VkShaderModule modules[2];
    for(unsigned i=0;i<2;++i) {
        VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(vs),.pCode=i?fs:vs};
        assert(vkCreateShaderModule(&d,&mi,NULL,&modules[i])==VK_SUCCESS);
    }
    struct VkPipelineLayout_T layout={.device=&d};
    VkAttachmentDescription pass_attachments[1]={{.format=VK_FORMAT_B8G8R8A8_UNORM}};
    struct ps5vk_subpass pass_subpasses[1]={
        {.color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},.depth={VK_ATTACHMENT_UNUSED,0}}};
    struct VkRenderPass_T pass={.device=&d,.attachment_count=1,.subpass_count=1,
        .attachments=pass_attachments,.subpasses=pass_subpasses};
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
    VkPipelineVertexInputStateCreateInfo v={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo r={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo m={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkViewport viewport={0,0,1920,1080,0,1}; VkRect2D scissor={{0,0},{1920,1080}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineColorBlendAttachmentState color={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo b={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&color};
    /* Vulkan ignores this state when the pipeline contains no tessellation
     * stages. The CTS helper supplies this otherwise-unused pointer. */
    VkPipelineTessellationStateCreateInfo tess={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.layout=&layout,.renderPass=&pass,
        .stageCount=2,.pStages=stages,.pVertexInputState=&v,.pInputAssemblyState=&ia,
        .pTessellationState=&tess,.pRasterizationState=&r,.pMultisampleState=&m,
        .pViewportState=&vp,.pColorBlendState=&b};
    VkPipeline p; assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&p)==VK_SUCCESS && p->graphics && created==1);
    /* A pipeline is created for ONE subpass and carries that identity, which
     * is what vkCmdDraw later checks the recording subpass against. */
    assert(!p->subpass);
    viewport.width=1; assert(p->viewport.width==1920);
    {
        /* The counters below are pinned by later assertions, so this block
         * saves and restores them: it exercises subpass identity, not the
         * backend accounting, and must be invisible to the rest. */
        const unsigned saved_created = created, saved_released = released;
        const unsigned saved_acquired = acquired, saved_compiled = compiled_released;

        /* A NONZERO subpass is accepted when the pass actually has it, and the
         * pipeline remembers which one. The profile requires every subpass to
         * name the same attachments, so the formats are necessarily the same
         * in both - the identity, not the format, is what differs here. */
        struct ps5vk_subpass two_subpasses[2]={pass_subpasses[0],pass_subpasses[0]};
        struct VkRenderPass_T two={.device=&d,.attachment_count=1,.subpass_count=2,
            .attachments=pass_attachments,.subpasses=two_subpasses};
        VkGraphicsPipelineCreateInfo second=info;
        second.renderPass=&two; second.subpass=1;
        VkPipeline later;
        assert(vkCreateGraphicsPipelines(&d,0,1,&second,NULL,&later)==VK_SUCCESS);
        assert(later->subpass==1 && later->color_format==VK_FORMAT_B8G8R8A8_UNORM);
        vkDestroyPipeline(&d,later,NULL);
        /* A subpass the pass does not have is refused rather than clamped. */
        second.subpass=2;
        assert(vkCreateGraphicsPipelines(&d,0,1,&second,NULL,&later)==
               VK_ERROR_FEATURE_NOT_PRESENT && !later);
        /* And a nonzero subpass against a one-subpass pass is equally out of
         * range, which is the case that used to be refused for the wrong
         * reason - because ANY nonzero index was rejected. */
        second.renderPass=&pass; second.subpass=1;
        assert(vkCreateGraphicsPipelines(&d,0,1,&second,NULL,&later)==
               VK_ERROR_FEATURE_NOT_PRESENT && !later);
        created = saved_created; released = saved_released;
        acquired = saved_acquired; compiled_released = saved_compiled;
    }
    VkDynamicState dynamic_values[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=2,.pDynamicStates=dynamic_values};
    vp.pViewports=NULL;vp.pScissors=NULL;info.pDynamicState=&dynamic;
    VkPipeline dynamic_pipeline;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&dynamic_pipeline)==VK_SUCCESS);
    assert(dynamic_pipeline->dynamic_viewport && dynamic_pipeline->dynamic_scissor);
    vkDestroyPipeline(&d,dynamic_pipeline,NULL);
    const VkDynamicState unsupported_dynamic[]={
        VK_DYNAMIC_STATE_LINE_WIDTH,VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    unsigned before_created=created;
    dynamic.dynamicStateCount=1;
    for(unsigned i=0;i<sizeof(unsupported_dynamic)/sizeof(unsupported_dynamic[0]);++i) {
        dynamic.pDynamicStates=&unsupported_dynamic[i];
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&dynamic_pipeline)==
            VK_ERROR_FEATURE_NOT_PRESENT && !dynamic_pipeline && created==before_created);
    }
    dynamic.dynamicStateCount=0;dynamic.pDynamicStates=NULL;
    vp.pViewports=&viewport;vp.pScissors=&scissor;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&dynamic_pipeline)==VK_SUCCESS);
    vkDestroyPipeline(&d,dynamic_pipeline,NULL);
    vp.pViewports=NULL;vp.pScissors=NULL;
    dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dynamic_values;
    dynamic_values[1]=VK_DYNAMIC_STATE_VIEWPORT;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&dynamic_pipeline)==VK_ERROR_FEATURE_NOT_PRESENT && !dynamic_pipeline);
    dynamic_values[1]=(VkDynamicState)0x7fffffff;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&dynamic_pipeline)==VK_ERROR_FEATURE_NOT_PRESENT && !dynamic_pipeline);
    dynamic_values[1]=VK_DYNAMIC_STATE_SCISSOR;
    vp.pViewports=&viewport;vp.pScissors=&scissor;info.pDynamicState=NULL;
    d.graphics_library=NULL;d.graphics_compiler_context=&acquired;
    d.graphics_acquire=acquire;d.graphics_compiled_release=compiled_release;
    VkPipeline runtime;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_SUCCESS);
    assert(acquired==1 && compiled_released==1);
    vkDestroyPipeline(&d,runtime,NULL);
    compile_fail=1;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_ERROR_FEATURE_NOT_PRESENT && !runtime);
    assert(acquired==2 && compiled_released==2 && created==4);
    compile_fail=0;backend_fail=1;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_ERROR_UNKNOWN && !runtime);
    assert(acquired==3 && compiled_released==3 && released==4);
    backend_fail=0;d.graphics_compiled_release=NULL;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_ERROR_FEATURE_NOT_PRESENT && !runtime);
    assert(acquired==3);
    d.graphics_compiled_release=compiled_release;
    VkVertexInputBindingDescription bindings[16];
    VkVertexInputAttributeDescription attributes[16];
    for(unsigned i=0;i<16;++i) {
        bindings[i]=(VkVertexInputBindingDescription){15-i,16+i,VK_VERTEX_INPUT_RATE_VERTEX};
        attributes[i]=(VkVertexInputAttributeDescription){i,15-i,VK_FORMAT_R32_SFLOAT,0};
    }
    v.vertexBindingDescriptionCount=v.vertexAttributeDescriptionCount=16;
    v.pVertexBindingDescriptions=bindings;v.pVertexAttributeDescriptions=attributes;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_SUCCESS);
    for(unsigned i=0;i<16;++i)bindings[i].stride=999;
    assert(runtime->vertex_binding_count==16 && runtime->vertex_attribute_count==16);
    for(unsigned i=0;i<16;++i)assert(runtime->vertex_bindings[i].binding==15-i &&
        runtime->vertex_bindings[i].stride==16+i);
    vkDestroyPipeline(&d,runtime,NULL);
    v.vertexBindingDescriptionCount=v.vertexAttributeDescriptionCount=0;
    v.pVertexBindingDescriptions=NULL;v.pVertexAttributeDescriptions=NULL;
    layout.set_count=4;
    for(unsigned s=0;s<4;++s) {
        layout.sets[s].count=24;
        layout.sets[s].binding[7]=(struct ps5vk_binding){24,0,VK_SHADER_STAGE_FRAGMENT_BIT};
        layout.sets[s].type[7]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        for(unsigned b=8;b<PS5VK_MAX_BINDINGS;++b)layout.sets[s].binding[b].first=24;
    }
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_SUCCESS && runtime->set_count==4);
    layout.sets[3].binding[7].count=23;
    assert(runtime->sets[3].binding[7].count==24); /* pipeline owns its signature */
    vkDestroyPipeline(&d,runtime,NULL);layout.set_count=0;
    d.graphics_acquire=NULL;d.graphics_library=&library;
    created=1;released=0;
    vkDestroyShaderModule(&d,modules[0],NULL); vkDestroyShaderModule(&d,modules[1],NULL);
    p->pending=1; vkDestroyPipeline(&d,p,NULL); assert(!released);
    p->pending=0; vkDestroyPipeline(&d,p,NULL); assert(released==1 && !d.pipeline_objects);
    d.graphics_create=NULL;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&p)==VK_ERROR_FEATURE_NOT_PRESENT && !p);
}

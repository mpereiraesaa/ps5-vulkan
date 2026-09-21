#include "vk_pipeline.h"
#include "vk_render_pass.h"
#include "graphics_program.h"
#include <assert.h>
#include <stdlib.h>
static unsigned created, released;
static unsigned acquired, compiled_released, compile_fail, backend_fail;
static unsigned expect_five_stages;
static unsigned expect_blend_state;
static unsigned expect_dual_blend_state;
static uint32_t reported_set_mask;
static VkResult usage_result;
static VkResult used_sets(VkDevice d,const void *state,uint32_t *mask)
{ assert(d && state && mask);*mask=reported_set_mask;return usage_result; }
static uint32_t expected_primitive=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST;
static VkResult backend(VkDevice d,const void *data,uint32_t primitive_type,void **out)
{ (void)d; assert(data); assert(primitive_type==expected_primitive); ++created; *out=malloc(1); return backend_fail?VK_ERROR_UNKNOWN:(*out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY); }
static void release(VkDevice d,void *data) { (void)d; ++released; free(data); }
static VkResult acquire(void *context,const struct ps5vk_graphics_key *key,const void **out)
{
    assert(context==&acquired && key->vertex.word_count==10 && key->fragment.word_count==10);
    if(expect_blend_state) {
        assert(key->blend_enable==VK_TRUE);
        assert(key->src_color_blend_factor==VK_BLEND_FACTOR_SRC_ALPHA);
        assert(key->dst_color_blend_factor==VK_BLEND_FACTOR_ONE);
        assert(key->color_blend_op==VK_BLEND_OP_ADD);
        assert(key->src_alpha_blend_factor==VK_BLEND_FACTOR_CONSTANT_ALPHA);
        assert(key->dst_alpha_blend_factor==VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        assert(key->alpha_blend_op==VK_BLEND_OP_REVERSE_SUBTRACT);
        for(unsigned i=0;i<4;++i) assert(key->blend_constants[i]==(float)i/4.0f);
    }
    if(expect_dual_blend_state) {
        assert(key->blend_enable==VK_TRUE);
        assert(key->feature_mask&PS5VK_FEATURE_DUAL_SRC_BLEND);
        assert(key->src_color_blend_factor==VK_BLEND_FACTOR_SRC1_COLOR);
        assert(key->dst_color_blend_factor==VK_BLEND_FACTOR_ZERO);
        assert(key->color_blend_op==VK_BLEND_OP_ADD);
        assert(key->src_alpha_blend_factor==VK_BLEND_FACTOR_ONE);
        assert(key->dst_alpha_blend_factor==VK_BLEND_FACTOR_ZERO);
        assert(key->alpha_blend_op==VK_BLEND_OP_ADD);
    }
    if(expect_five_stages) {
        assert(key->tess_control.word_count==10 && key->tess_eval.word_count==10 &&
               key->geometry.word_count==10 && key->patch_control_points==3);
    }
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
        {.color={{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}},.color_count=1,
         .depth={VK_ATTACHMENT_UNUSED,0}}};
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
    {
        /* The rasterization state's pNext chain. The pinned upstream
         * rasterization module chains VkPipelineRasterizationLineStateCreateInfoEXT
         * unconditionally - sType set even without VK_EXT_line_rasterization,
         * which this device does not expose - and the exact form it supplies
         * asks only for the default rectangular mode with no stipple, which is
         * what this driver already does. That one form is accepted; every other
         * chain and every other value stays refused, because accepting a
         * structure that asks for different rasterization would change the
         * output this profile claims to produce. */
        const unsigned saved_created = created, saved_released = released;
        const unsigned saved_acquired = acquired, saved_compiled = compiled_released;
        VkPipelineRasterizationLineStateCreateInfoEXT line={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT,
            .pNext=NULL,
            .lineRasterizationMode=VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT,
            .stippledLineEnable=VK_FALSE};
        VkPipelineRasterizationStateCreateInfo chained=r;
        chained.pNext=&line;
        VkGraphicsPipelineCreateInfo with_chain=info;
        with_chain.pRasterizationState=&chained;
        VkPipeline chained_pipeline=NULL;
        assert(vkCreateGraphicsPipelines(&d,0,1,&with_chain,NULL,&chained_pipeline)==VK_SUCCESS &&
            chained_pipeline && chained_pipeline->graphics);
        vkDestroyPipeline(&d,chained_pipeline,NULL);
        line.stippledLineEnable=VK_TRUE;
        assert(vkCreateGraphicsPipelines(&d,0,1,&with_chain,NULL,&chained_pipeline)==
            VK_ERROR_FEATURE_NOT_PRESENT && !chained_pipeline);
        line.stippledLineEnable=VK_FALSE;
        line.lineRasterizationMode=VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
        assert(vkCreateGraphicsPipelines(&d,0,1,&with_chain,NULL,&chained_pipeline)==
            VK_ERROR_FEATURE_NOT_PRESENT && !chained_pipeline);
        line.lineRasterizationMode=VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT;
        VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT, .pNext=NULL};
        line.pNext=&provoking;
        assert(vkCreateGraphicsPipelines(&d,0,1,&with_chain,NULL,&chained_pipeline)==
            VK_ERROR_FEATURE_NOT_PRESENT && !chained_pipeline);
        line.pNext=NULL;
        VkPipelineRasterizationProvokingVertexStateCreateInfoEXT unknown={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT, .pNext=NULL};
        VkPipelineRasterizationStateCreateInfo other_chain=r;
        other_chain.pNext=&unknown;
        with_chain.pRasterizationState=&other_chain;
        assert(vkCreateGraphicsPipelines(&d,0,1,&with_chain,NULL,&chained_pipeline)==
            VK_ERROR_FEATURE_NOT_PRESENT && !chained_pipeline);
        created=saved_created;
        released=saved_released;
        acquired=saved_acquired;
        compiled_released=saved_compiled;
    }
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
    {
        /* Depth bias is real state: the enable and both factors are carried
         * on the pipeline. A non-zero clamp needs depthBiasClamp ENABLED on
         * the logical device; the factors are never subject to a finiteness
         * rule, and a disabled bias stores zero factors whatever was passed. */
        const unsigned saved_created=created,saved_released=released;
        const unsigned saved_acquired=acquired,saved_compiled=compiled_released;
        VkPipeline biased=NULL;
        r.depthBiasEnable=VK_TRUE;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->raster.depth_bias_enable && !biased->dynamic_depth_bias &&
            biased->raster.depth_bias_constant==0.0f && biased->raster.depth_bias_slope==0.0f &&
            biased->raster.depth_bias_clamp==0.0f);
        vkDestroyPipeline(&d,biased,NULL);
        r.depthBiasConstantFactor=0.25f;r.depthBiasSlopeFactor=-1.0f;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->raster.depth_bias_enable && biased->raster.depth_bias_constant==0.25f &&
            biased->raster.depth_bias_slope==-1.0f && biased->raster.depth_bias_clamp==0.0f);
        vkDestroyPipeline(&d,biased,NULL);
        /* Clamp without the feature: refused, no backend object created. */
        r.depthBiasClamp=0.5f;
        const unsigned before=created;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased && created==before);
        /* Clamp with the feature enabled on the device: carried as given. */
        d.enabled_features|=PS5VK_FEATURE_DEPTH_BIAS_CLAMP;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->raster.depth_bias_clamp==0.5f && biased->raster.depth_bias_constant==0.25f);
        vkDestroyPipeline(&d,biased,NULL);
        d.enabled_features&=~PS5VK_FEATURE_DEPTH_BIAS_CLAMP;
        /* Disabled bias ignores every factor, the clamp included. */
        r.depthBiasEnable=VK_FALSE;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(!biased->raster.depth_bias_enable && biased->raster.depth_bias_constant==0.0f &&
            biased->raster.depth_bias_slope==0.0f && biased->raster.depth_bias_clamp==0.0f);
        vkDestroyPipeline(&d,biased,NULL);
        /* depthClampEnable: refused without depthClamp enabled on the device
         * (no backend object), carried as static raster state with it. */
        r.depthClampEnable=VK_TRUE;
        const unsigned before_clamp=created;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased && created==before_clamp);
        d.enabled_features|=PS5VK_FEATURE_DEPTH_CLAMP;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->raster.depth_clamp && !biased->raster.depth_bias_enable);
        vkDestroyPipeline(&d,biased,NULL);
        d.enabled_features&=~PS5VK_FEATURE_DEPTH_CLAMP;
        r.depthClampEnable=VK_FALSE;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(!biased->raster.depth_clamp);
        vkDestroyPipeline(&d,biased,NULL);
        /* polygonMode: LINE and POINT need fillModeNonSolid enabled on the
         * device, FILL_RECTANGLE_NV is refused with or without it, FILL is
         * always accepted; the mode is carried as static raster state. */
        r.polygonMode=VK_POLYGON_MODE_LINE;
        const unsigned before_poly=created;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased && created==before_poly);
        r.polygonMode=VK_POLYGON_MODE_POINT;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased && created==before_poly);
        d.enabled_features|=PS5VK_FEATURE_FILL_MODE_NON_SOLID;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->raster.polygon_mode==VK_POLYGON_MODE_POINT);
        vkDestroyPipeline(&d,biased,NULL);
        r.polygonMode=VK_POLYGON_MODE_LINE;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->raster.polygon_mode==VK_POLYGON_MODE_LINE);
        vkDestroyPipeline(&d,biased,NULL);
        r.polygonMode=VK_POLYGON_MODE_FILL_RECTANGLE_NV;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased);
        /* wideLines is not advertised: a non-solid pipeline still needs 1.0. */
        r.polygonMode=VK_POLYGON_MODE_LINE;r.lineWidth=2.0f;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased);
        r.lineWidth=1.0f;
        d.enabled_features&=~PS5VK_FEATURE_FILL_MODE_NON_SOLID;
        r.polygonMode=VK_POLYGON_MODE_FILL;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->raster.polygon_mode==VK_POLYGON_MODE_FILL);
        vkDestroyPipeline(&d,biased,NULL);
        /* Viewport arrays: counts must match and stay in 1..16; more than
         * one needs multiViewport enabled; every static element is validated
         * and all of them are stored, in order. */
        VkViewport many_viewports[PS5VK_MAX_VIEWPORTS+1]; VkRect2D many_scissors[PS5VK_MAX_VIEWPORTS+1];
        for(unsigned i=0;i<=PS5VK_MAX_VIEWPORTS;++i) {
            many_viewports[i]=(VkViewport){(float)i,0,64,32,0,1};
            many_scissors[i]=(VkRect2D){{(int32_t)i,0},{64,32}};
        }
        vp.pViewports=many_viewports;vp.pScissors=many_scissors;
        vp.viewportCount=2;vp.scissorCount=2;
        const unsigned before_vp=created;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased && created==before_vp);
        d.enabled_features|=PS5VK_FEATURE_MULTI_VIEWPORT;
        vp.scissorCount=3;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased);
        vp.viewportCount=vp.scissorCount=0;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased);
        vp.viewportCount=vp.scissorCount=PS5VK_MAX_VIEWPORTS+1;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==
            VK_ERROR_FEATURE_NOT_PRESENT && !biased);
        vp.viewportCount=vp.scissorCount=PS5VK_MAX_VIEWPORTS;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->viewport_count==PS5VK_MAX_VIEWPORTS && biased->viewport.x==0 &&
            biased->viewports[15].x==15 && biased->scissors[15].offset.x==15 &&
            biased->viewports[7].width==64 && biased->scissors[7].extent.height==32);
        vkDestroyPipeline(&d,biased,NULL);
        /* A bad element in the LAST slot fails creation before anything is kept. */
        many_viewports[15].width=0;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_ERROR_UNKNOWN && !biased);
        many_viewports[15].width=64;many_scissors[15].offset.y=-1;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_ERROR_UNKNOWN && !biased);
        many_scissors[15].offset.y=0;
        /* Dynamic arrays keep the count static and skip element validation. */
        VkDynamicState array_dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo array_state={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount=2,.pDynamicStates=array_dynamic};
        info.pDynamicState=&array_state;vp.pViewports=NULL;vp.pScissors=NULL;vp.viewportCount=vp.scissorCount=4;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->viewport_count==4 && biased->dynamic_viewport && biased->dynamic_scissor);
        vkDestroyPipeline(&d,biased,NULL);
        info.pDynamicState=NULL;
        d.enabled_features&=~PS5VK_FEATURE_MULTI_VIEWPORT;
        vp.pViewports=&viewport;vp.pScissors=&scissor;vp.viewportCount=vp.scissorCount=1;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->viewport_count==1);
        vkDestroyPipeline(&d,biased,NULL);
        /* Dynamic depth bias: the enable stays static, the factors are not
         * read from the create info (a non-zero clamp here is ignored too),
         * and VK_DYNAMIC_STATE_DEPTH_BIAS is accepted next to the other two. */
        VkDynamicState bias_dynamic[]={VK_DYNAMIC_STATE_DEPTH_BIAS,VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo bias_state={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount=1,.pDynamicStates=bias_dynamic};
        info.pDynamicState=&bias_state;r.depthBiasEnable=VK_TRUE;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->dynamic_depth_bias && biased->raster.depth_bias_enable &&
            biased->raster.depth_bias_clamp==0.0f && biased->raster.depth_bias_constant==0.0f &&
            !biased->dynamic_viewport && !biased->dynamic_scissor);
        vkDestroyPipeline(&d,biased,NULL);
        bias_state.dynamicStateCount=3;vp.pViewports=NULL;vp.pScissors=NULL;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_SUCCESS);
        assert(biased->dynamic_depth_bias && biased->dynamic_viewport && biased->dynamic_scissor);
        vkDestroyPipeline(&d,biased,NULL);
        /* A repeated dynamic state is still refused. */
        bias_dynamic[2]=VK_DYNAMIC_STATE_DEPTH_BIAS;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&biased)==VK_ERROR_FEATURE_NOT_PRESENT && !biased);
        vp.pViewports=&viewport;vp.pScissors=&scissor;info.pDynamicState=NULL;
        r.depthBiasClamp=0.0f;r.depthBiasConstantFactor=0.0f;r.depthBiasSlopeFactor=0.0f;
        r.depthBiasEnable=VK_FALSE;
        created=saved_created;released=saved_released;
        acquired=saved_acquired;compiled_released=saved_compiled;
    }
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=2,.pDynamicStates=dynamic_values};
    vp.pViewports=NULL;vp.pScissors=NULL;info.pDynamicState=&dynamic;
    VkPipeline dynamic_pipeline;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&dynamic_pipeline)==VK_SUCCESS);
    assert(dynamic_pipeline->dynamic_viewport && dynamic_pipeline->dynamic_scissor);
    vkDestroyPipeline(&d,dynamic_pipeline,NULL);
    const VkDynamicState unsupported_dynamic[]={
        VK_DYNAMIC_STATE_LINE_WIDTH,
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
    {
        unsigned a=acquired,built=created,freed=released,leases=compiled_released;
        d.graphics_used_sets=used_sets;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_SUCCESS);
        assert(runtime->graphics_usage_known && !runtime->graphics_used_set_mask);
        vkDestroyPipeline(&d,runtime,NULL);
        reported_set_mask=1; /* outside this empty layout */
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)!=VK_SUCCESS && !runtime);
        reported_set_mask=0;usage_result=VK_ERROR_UNKNOWN;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_ERROR_UNKNOWN && !runtime);
        assert(created-built==3 && released-freed==3 && compiled_released-leases==3);
        usage_result=VK_SUCCESS;d.graphics_used_sets=NULL;
        acquired=a;created=built;released=freed;compiled_released=leases;
    }
    {
        /* This mock validates transmission only, not hardware blend support. */
        unsigned saved_a=acquired,saved_c=compiled_released;
        unsigned saved_created=created,saved_released=released;
        color.blendEnable=VK_TRUE;
        color.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        color.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;
        color.srcAlphaBlendFactor=VK_BLEND_FACTOR_CONSTANT_ALPHA;
        color.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color.alphaBlendOp=VK_BLEND_OP_REVERSE_SUBTRACT;
        for(unsigned i=0;i<4;++i) b.blendConstants[i]=(float)i/4.0f;
        expect_blend_state=1;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_SUCCESS);
        assert(runtime->color_blend.blendEnable==VK_TRUE &&
            runtime->color_blend.srcColorBlendFactor==VK_BLEND_FACTOR_SRC_ALPHA &&
            runtime->color_blend.dstColorBlendFactor==VK_BLEND_FACTOR_ONE &&
            runtime->color_blend.srcAlphaBlendFactor==VK_BLEND_FACTOR_CONSTANT_ALPHA &&
            runtime->color_blend.dstAlphaBlendFactor==VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
            runtime->color_blend.alphaBlendOp==VK_BLEND_OP_REVERSE_SUBTRACT);
        for(unsigned i=0;i<4;++i) assert(runtime->blend_constants[i]==(float)i/4.0f);
        vkDestroyPipeline(&d,runtime,NULL);
        expect_blend_state=0;
        /* SRC1 is rejected before acquisition unless dualSrcBlend was enabled
         * on the logical device.  With the bit enabled, the exact equation is
         * transmitted unchanged; compiler/export validation is covered by the
         * real-compiler test rather than this mock backend. */
        color.srcColorBlendFactor=VK_BLEND_FACTOR_SRC1_COLOR;
        color.dstColorBlendFactor=VK_BLEND_FACTOR_ZERO;
        color.colorBlendOp=VK_BLEND_OP_ADD;
        color.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        color.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
        color.alphaBlendOp=VK_BLEND_OP_ADD;
        const unsigned before_dual_acquire=acquired;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==
            VK_ERROR_FEATURE_NOT_PRESENT && !runtime && acquired==before_dual_acquire);
        d.enabled_features|=PS5VK_FEATURE_DUAL_SRC_BLEND;
        expect_dual_blend_state=1;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&runtime)==VK_SUCCESS);
        assert(runtime->color_blend.srcColorBlendFactor==VK_BLEND_FACTOR_SRC1_COLOR &&
            runtime->color_blend.dstColorBlendFactor==VK_BLEND_FACTOR_ZERO);
        vkDestroyPipeline(&d,runtime,NULL);
        expect_dual_blend_state=0;
        d.enabled_features&=~PS5VK_FEATURE_DUAL_SRC_BLEND;
        color=(VkPipelineColorBlendAttachmentState){.colorWriteMask=15};
        for(unsigned i=0;i<4;++i) b.blendConstants[i]=0;
        acquired=saved_a;compiled_released=saved_c;
        created=saved_created;released=saved_released;
    }
    {
        /* Topology selects the primitive the backend links. Both accepted
         * topologies reach the backend with their pinned GFX1013 value, and an
         * unsupported topology is refused before any backend work happens.
         * The counters below are pinned by later assertions, so this block
         * restores them and uses the runtime lease path, where a program does
         * not have to come from an offline record of the same topology. */
        const unsigned saved_created=created,saved_released=released;
        const unsigned saved_acquired=acquired,saved_compiled=compiled_released;
        VkPipeline topo=NULL;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        expected_primitive=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&topo)==VK_SUCCESS && topo->graphics);
        vkDestroyPipeline(&d,topo,NULL);
        const VkPrimitiveTopology unsupported_topologies[]={
            VK_PRIMITIVE_TOPOLOGY_POINT_LIST,VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY};
        unsigned before_created=created;
        for(unsigned i=0;i<sizeof(unsupported_topologies)/sizeof(unsupported_topologies[0]);++i) {
            ia.topology=unsupported_topologies[i];
            assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&topo)==
                VK_ERROR_FEATURE_NOT_PRESENT && !topo && created==before_created);
        }
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        expected_primitive=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&topo)==VK_SUCCESS);
        vkDestroyPipeline(&d,topo,NULL);
        /* Primitive restart is input-assembly state the front end can only act
         * on across a strip, and the pinned conformance geometry module declares
         * it exactly for the strips (vktGeometryTestsUtil.cpp:153-172). It is
         * accepted on a strip - and recorded on the pipeline, because the draw
         * path programs the cut from that flag and the draw's index width - and
         * refused on a list, where a restart index could not do what the caller
         * declared. */
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        expected_primitive=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_STRIP;
        ia.primitiveRestartEnable=VK_TRUE;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&topo)==VK_SUCCESS && topo->graphics);
        assert(topo->primitive_restart==VK_TRUE);
        vkDestroyPipeline(&d,topo,NULL);
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        expected_primitive=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST;
        before_created=created;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&topo)==
            VK_ERROR_FEATURE_NOT_PRESENT && !topo && created==before_created);
        ia.primitiveRestartEnable=VK_FALSE;
        created=saved_created;released=saved_released;
        acquired=saved_acquired;compiled_released=saved_compiled;
    }
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
    d.graphics_acquire=NULL;d.graphics_compiled_release=NULL;
    d.graphics_compiler_context=NULL;d.graphics_library=&library;
    /* The optional geometry stage: refused unless the logical device enabled the
     * feature, and then matched against a record that carries the same geometry
     * module, so a two-stage program can never satisfy a three-stage pipeline. */
    {
        uint32_t gs[]={0x07230203,0x10000,0,2,0,(5u<<16)|15,3,1,0x6e69616d,0};
        VkShaderModule geometry_module;
        VkShaderModuleCreateInfo gmi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=sizeof(gs),.pCode=gs};
        assert(vkCreateShaderModule(&d,&gmi,NULL,&geometry_module)==VK_SUCCESS);
        VkPipelineShaderStageCreateInfo geometry_stages[3]={
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_GEOMETRY_BIT,.module=geometry_module,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
        VkGraphicsPipelineCreateInfo geometry_info=info;
        geometry_info.stageCount=3;geometry_info.pStages=geometry_stages;
        VkPipeline geometry_pipeline;
        assert(vkCreateGraphicsPipelines(&d,0,1,&geometry_info,NULL,&geometry_pipeline)==
               VK_ERROR_FEATURE_NOT_PRESENT);
        d.enabled_features|=PS5VK_FEATURE_GEOMETRY_SHADER;
        assert(vkCreateGraphicsPipelines(&d,0,1,&geometry_info,NULL,&geometry_pipeline)==
               VK_ERROR_FEATURE_NOT_PRESENT);
        struct ps5vk_graphics_program geometry_program=program;
        geometry_program.key.geometry=(struct ps5vk_graphics_module_key){
            .words=gs,.word_count=10,.entry="main"};
        struct ps5vk_graphics_library geometry_library={&geometry_program,1};
        d.graphics_library=&geometry_library;
        assert(vkCreateGraphicsPipelines(&d,0,1,&geometry_info,NULL,&geometry_pipeline)==
               VK_SUCCESS && geometry_pipeline->graphics);
        vkDestroyPipeline(&d,geometry_pipeline,NULL);
        /* A two-stage pipeline still matches only the record without a geometry
         * module: the optional stage is part of the program identity. */
        d.graphics_library=&library;
        VkPipeline two_stage;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&two_stage)==VK_SUCCESS);
        vkDestroyPipeline(&d,two_stage,NULL);
        d.enabled_features&=~PS5VK_FEATURE_GEOMETRY_SHADER;
        vkDestroyShaderModule(&d,geometry_module,NULL);
    }
    /* The tessellation contract: the control and evaluation stages are
     * described and validated, and the pipeline is then refused because the
     * pinned compiler emits no loadable package for them. PATCH_LIST without
     * them, a missing or out-of-range patchControlPoints, and the stages
     * without PATCH_LIST are all refused. */
    {
        uint32_t tcs_words[]={0x07230203,0x10000,0,2,0,(5u<<16)|15,1,1,0x6e69616d,0};
        uint32_t tes_words[]={0x07230203,0x10000,0,2,0,(5u<<16)|15,2,1,0x6e69616d,0};
        VkShaderModule tcs_module,tes_module;
        VkShaderModuleCreateInfo tcs_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=sizeof(tcs_words),.pCode=tcs_words};
        VkShaderModuleCreateInfo tes_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=sizeof(tes_words),.pCode=tes_words};
        assert(vkCreateShaderModule(&d,&tcs_info,NULL,&tcs_module)==VK_SUCCESS);
        assert(vkCreateShaderModule(&d,&tes_info,NULL,&tes_module)==VK_SUCCESS);
        VkPipelineShaderStageCreateInfo tess_stages[4]={
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,.module=tcs_module,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,.module=tes_module,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
        VkPipelineTessellationStateCreateInfo tessellation={
            .sType=VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
            .patchControlPoints=3};
        VkGraphicsPipelineCreateInfo tess_info=info;
        tess_info.stageCount=4;tess_info.pStages=tess_stages;
        tess_info.pTessellationState=&tessellation;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        tess_info.pInputAssemblyState=&ia;
        VkPipeline tess_pipeline;
        assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)==
               VK_ERROR_FEATURE_NOT_PRESENT);
        {
            uint32_t gs_words[]={0x07230203,0x10000,0,2,0,(5u<<16)|15,3,1,0x6e69616d,0};
            VkShaderModule gs_module;
            VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize=sizeof(gs_words),.pCode=gs_words};
            assert(vkCreateShaderModule(&d,&mi,NULL,&gs_module)==VK_SUCCESS);
            VkPipelineShaderStageCreateInfo five[5];
            for(unsigned i=0;i<4;++i)five[i]=tess_stages[i];
            five[4]=(VkPipelineShaderStageCreateInfo){
                .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage=VK_SHADER_STAGE_GEOMETRY_BIT,.module=gs_module,.pName="main"};
            tess_info.stageCount=5;tess_info.pStages=five;
            const uint64_t saved_features=d.enabled_features;
            d.graphics_acquire=acquire;d.graphics_compiled_release=compiled_release;
            d.graphics_compiler_context=&acquired;
            expect_five_stages=1;expected_primitive=9u;
            const uint64_t required=PS5VK_FEATURE_GEOMETRY_SHADER|PS5VK_FEATURE_TESSELLATION_SHADER;
            d.enabled_features=saved_features|required;
            assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)==VK_SUCCESS);
            vkDestroyPipeline(&d,tess_pipeline,NULL);
            for(unsigned i=0;i<2;++i) {
                d.enabled_features=(saved_features|required)&~(i?PS5VK_FEATURE_GEOMETRY_SHADER:PS5VK_FEATURE_TESSELLATION_SHADER);
                unsigned calls=acquired;tess_pipeline=(void *)(uintptr_t)1;
                assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)==VK_ERROR_FEATURE_NOT_PRESENT);
                assert(!tess_pipeline && acquired==calls);
            }
            d.enabled_features=saved_features|required;
            five[4]=five[0];
            assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)!=VK_SUCCESS);
            d.enabled_features=saved_features;expect_five_stages=0;
            expected_primitive=PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST;
            d.graphics_acquire=NULL;d.graphics_compiled_release=NULL;d.graphics_compiler_context=NULL;
            tess_info.stageCount=4;tess_info.pStages=tess_stages;
            vkDestroyShaderModule(&d,gs_module,NULL);
        }
        /* The evaluation stage alone, and a patch list without them, are not a
         * tessellation pipeline either. */
        tess_info.stageCount=3;
        tess_info.pStages=&tess_stages[1];
        assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)!=
               VK_SUCCESS);
        tess_info.stageCount=4;tess_info.pStages=tess_stages;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)==
               VK_ERROR_FEATURE_NOT_PRESENT);
        ia.topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        VkPipelineTessellationStateCreateInfo bad=tessellation;
        bad.patchControlPoints=0;
        tess_info.pTessellationState=&bad;
        assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)==
               VK_ERROR_FEATURE_NOT_PRESENT);
        bad.patchControlPoints=PS5VK_MAX_PATCH_CONTROL_POINTS+1;
        assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)==
               VK_ERROR_FEATURE_NOT_PRESENT);
        tess_info.pTessellationState=&tessellation;
        tess_info.pTessellationState=NULL;
        assert(vkCreateGraphicsPipelines(&d,0,1,&tess_info,NULL,&tess_pipeline)==
               VK_ERROR_FEATURE_NOT_PRESENT);
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&tess_pipeline)==VK_SUCCESS);
        vkDestroyPipeline(&d,tess_pipeline,NULL);
        vkDestroyShaderModule(&d,tcs_module,NULL);
        vkDestroyShaderModule(&d,tes_module,NULL);
    }
    created=1;released=0;
    vkDestroyShaderModule(&d,modules[0],NULL); vkDestroyShaderModule(&d,modules[1],NULL);
    p->pending=1; vkDestroyPipeline(&d,p,NULL); assert(!released);
    p->pending=0; vkDestroyPipeline(&d,p,NULL); assert(released==1 && !d.pipeline_objects);
    d.graphics_create=NULL;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&p)==VK_ERROR_FEATURE_NOT_PRESENT && !p);
}

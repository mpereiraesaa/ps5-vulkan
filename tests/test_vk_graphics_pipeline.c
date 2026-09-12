#include "vk_pipeline.h"
#include "vk_render_pass.h"
#include "graphics_program.h"
#include <assert.h>
#include <stdlib.h>
static unsigned created, released;
static VkResult backend(VkDevice d,const void *data,void **out)
{ (void)d; assert(data); ++created; *out=malloc(1); return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release(VkDevice d,void *data) { (void)d; ++released; free(data); }
int main(void)
{
    uint32_t vs[]={0x07230203,0x10000,0,2,0,(5u<<16)|15,0,1,0x6e69616d,0};
    uint32_t fs[]={0x07230203,0x10000,0,2,0,(5u<<16)|15,4,1,0x6e69616d,0};
    struct ps5vk_graphics_program program={.key={.vertex={vs,10,"main"},.fragment={fs,10,"main"},
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
    struct VkRenderPass_T pass={.device=&d,.attachment_count=1,.color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},.depth={VK_ATTACHMENT_UNUSED,0}};
    pass.attachments[0].format=VK_FORMAT_B8G8R8A8_UNORM;
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
    VkGraphicsPipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.layout=&layout,.renderPass=&pass,
        .stageCount=2,.pStages=stages,.pVertexInputState=&v,.pInputAssemblyState=&ia,.pRasterizationState=&r,.pMultisampleState=&m,.pViewportState=&vp,.pColorBlendState=&b};
    VkPipeline p; assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&p)==VK_SUCCESS && p->graphics && created==1);
    viewport.width=1; assert(p->viewport.width==1920);
    vkDestroyShaderModule(&d,modules[0],NULL); vkDestroyShaderModule(&d,modules[1],NULL);
    p->pending=1; vkDestroyPipeline(&d,p,NULL); assert(!released);
    p->pending=0; vkDestroyPipeline(&d,p,NULL); assert(released==1 && !d.pipeline_objects);
    d.graphics_create=NULL;
    assert(vkCreateGraphicsPipelines(&d,0,1,&info,NULL,&p)==VK_ERROR_FEATURE_NOT_PRESENT && !p);
}

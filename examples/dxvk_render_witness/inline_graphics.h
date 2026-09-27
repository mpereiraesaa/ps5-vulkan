/* SPDX-License-Identifier: GPL-3.0-or-later
 * Shared public-SDK descriptor setup for the inline graphics witness.
 * The caller owns returned objects, including on partial failure. */
#ifndef PS5VK_INLINE_GRAPHICS_WITNESS_H
#define PS5VK_INLINE_GRAPHICS_WITNESS_H
static VkResult inline_graphics_descriptors(VkDevice device,
    VkDescriptorSetLayout *layout, VkDescriptorPool *pool, VkDescriptorSet *set,
    VkShaderStageFlags boundary_stage)
{
    const uint32_t blocks=boundary_stage?4:2, bytes=boundary_stage?1024:24;
    VkDescriptorSetLayoutBinding bindings[4] = {
        {0,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,20,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,4,VK_SHADER_STAGE_FRAGMENT_BIT,NULL}};
    if(boundary_stage) {
        if(boundary_stage!=VK_SHADER_STAGE_VERTEX_BIT && boundary_stage!=VK_SHADER_STAGE_FRAGMENT_BIT &&
           boundary_stage!=VK_SHADER_STAGE_GEOMETRY_BIT)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        for(unsigned b=0;b<4;++b) bindings[b]=(VkDescriptorSetLayoutBinding){
            b,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,256,boundary_stage,NULL};
    }
    VkDescriptorSetLayoutCreateInfo li = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=blocks,.pBindings=bindings};
    VkResult result=vkCreateDescriptorSetLayout(device,&li,NULL,layout);
    if(result!=VK_SUCCESS) return result;
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,bytes};
    VkDescriptorPoolInlineUniformBlockCreateInfo ip={
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO,
        .maxInlineUniformBlockBindings=blocks};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext=&ip,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    result=vkCreateDescriptorPool(device,&pi,NULL,pool);
    if(result!=VK_SUCCESS) return result;
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=layout};
    result=vkAllocateDescriptorSets(device,&ai,set);
    if(result!=VK_SUCCESS) return result;
    uint32_t data[256]={4,4,64,255,0x13579bdfu,0x2468ace0u};
    if(boundary_stage) for(unsigned i=0;i<256;++i) data[i]=0x4b000001u+i*37u;
    VkWriteDescriptorSetInlineUniformBlock inline_write={
        .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
        .dataSize=bytes,.pData=data};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&inline_write,
        .dstSet=*set,.descriptorCount=bytes,.descriptorType=VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK};
    vkUpdateDescriptorSets(device,1,&write,0,NULL); /* Rollover across all blocks. */
    volatile uint32_t *poison=data;
    for(unsigned i=0;i<bytes/4;++i) poison[i]=0;
    return VK_SUCCESS;
}
#endif

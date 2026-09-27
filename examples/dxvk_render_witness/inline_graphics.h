/* SPDX-License-Identifier: GPL-3.0-or-later
 * Shared public-SDK descriptor setup for the inline graphics witness.
 * The caller owns returned objects, including on partial failure. */
#ifndef PS5VK_INLINE_GRAPHICS_WITNESS_H
#define PS5VK_INLINE_GRAPHICS_WITNESS_H
static VkResult inline_graphics_descriptors(VkDevice device,
    VkDescriptorSetLayout *layout, VkDescriptorPool *pool, VkDescriptorSet *set)
{
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,20,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,4,VK_SHADER_STAGE_FRAGMENT_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2,.pBindings=bindings};
    VkResult result=vkCreateDescriptorSetLayout(device,&li,NULL,layout);
    if(result!=VK_SUCCESS) return result;
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,24};
    VkDescriptorPoolInlineUniformBlockCreateInfo ip={
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO,
        .maxInlineUniformBlockBindings=2};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext=&ip,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    result=vkCreateDescriptorPool(device,&pi,NULL,pool);
    if(result!=VK_SUCCESS) return result;
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=layout};
    result=vkAllocateDescriptorSets(device,&ai,set);
    if(result!=VK_SUCCESS) return result;
    uint32_t data[6]={4,4,64,255,0x13579bdfu,0x2468ace0u};
    VkWriteDescriptorSetInlineUniformBlock inline_write={
        .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
        .dataSize=sizeof(data),.pData=data};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&inline_write,
        .dstSet=*set,.descriptorCount=sizeof(data),.descriptorType=VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK};
    vkUpdateDescriptorSets(device,1,&write,0,NULL); /* Rollover across 20/4. */
    volatile uint32_t *poison=data;
    for(unsigned i=0;i<6;++i) poison[i]=0;
    return VK_SUCCESS;
}
#endif

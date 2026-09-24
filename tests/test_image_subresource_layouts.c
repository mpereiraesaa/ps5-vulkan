/* Public barriers execute in queue order, independently for each mip/layer. */
#define main image_copy_clear_regression_main
#include "test_image_copy_clear.c"
#undef main
/* Image barriers emitted by the original CopyImageToImage instance and
 * readImage helper, for all four optimal/general layout combinations. This
 * checks their public recording/execution contract, not the copy oracle. */
static void original_bc_image_copy_barriers(void)
{
    const VkImageLayout source_layouts[]={VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL};
    const VkImageLayout destination_layouts[]={VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL};
    VkImage source=make_image_subresources(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        256,256,1,1,VK_IMAGE_TILING_OPTIMAL,NULL);
    VkImage destination=make_image_subresources(VK_FORMAT_BC4_SNORM_BLOCK,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        256,256,1,1,VK_IMAGE_TILING_OPTIMAL,NULL);
    for(unsigned si=0;si<2;++si)for(unsigned di=0;di<2;++di) {
        VkImageMemoryBarrier barriers[2]={
            transfer_barrier(source,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0,VK_ACCESS_TRANSFER_WRITE_BIT),
            transfer_barrier(destination,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0,VK_ACCESS_TRANSFER_WRITE_BIT)};
        VkCommandBuffer command=begin();
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,2,barriers);
        for(unsigned i=0;i<2;++i) {
            barriers[i].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[i].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        /* The original upload helper retains DST with a WRITE->WRITE scope. */
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,2,barriers);
        barriers[0].newLayout=source_layouts[si];
        barriers[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].newLayout=destination_layouts[di];
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,2,barriers);
        submit_and_wait(command);
        assert(ps5vk_image_layout_matches(source,0,0,1,source_layouts[si]));
        assert(ps5vk_image_layout_matches(destination,0,0,1,destination_layouts[di]));
        vkFreeCommandBuffers(device,pool,1,&command);
        command=begin();
        barriers[1].oldLayout=destination_layouts[di];
        barriers[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,1,&barriers[1]);
        barriers[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[1].newLayout=destination_layouts[di];
        barriers[1].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,0,NULL,1,&barriers[1]);
        submit_and_wait(command);
        assert(ps5vk_image_layout_matches(destination,0,0,1,destination_layouts[di]));
        vkFreeCommandBuffers(device,pool,1,&command);
    }
    VkDeviceMemory memory=source->memory;
    vkDestroyImage(device,source,NULL);vkFreeMemory(device,memory,NULL);
    memory=destination->memory;
    vkDestroyImage(device,destination,NULL);vkFreeMemory(device,memory,NULL);
}

int main(void)
{
    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    device->graphics_enabled = VK_TRUE; /* the native platform installs these during configure */
    device->image_requirements = ps5vk_native_image_requirements;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &pci, NULL, &pool) == VK_SUCCESS);

    VkImage image=make_image_subresources(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        13,9,4,3,VK_IMAGE_TILING_OPTIMAL,NULL);
    assert(image->subresource_layouts);
    VkImageMemoryBarrier b=transfer_barrier(image,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    b.subresourceRange.levelCount=VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount=VK_REMAINING_ARRAY_LAYERS;
    VkCommandBuffer first=begin();
    vkCmdPipelineBarrier(first,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    /* Upstream copyBufferToImage emits this dependency even when its final
     * layout stays TRANSFER_DST_OPTIMAL: access scopes order transfer work
     * independently of whether a layout transition happens. */
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(first,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    /* Recording never changes committed state. */
    assert(image->layout==VK_IMAGE_LAYOUT_UNDEFINED);
    VkCommandBuffer second=begin();
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    b.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,2,1,1,1};
    vkCmdPipelineBarrier(second,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    assert(vkEndCommandBuffer(first)==VK_SUCCESS);
    assert(vkEndCommandBuffer(second)==VK_SUCCESS);
    VkCommandBuffer commands[2]={first,second};
    VkSubmitInfo submission={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=2,.pCommandBuffers=commands};
    assert(vkQueueSubmit(&device->queue,1,&submission,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(vkQueueWaitIdle(&device->queue)==VK_SUCCESS);
    assert(image->layout==VK_IMAGE_LAYOUT_MAX_ENUM);
    for(unsigned m=0;m<4;++m)for(unsigned l=0;l<3;++l)
        assert(ps5vk_image_layout_matches(image,m,l,1,m==2&&l==1?
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
    VkImageLayout snapshot[12];memcpy(snapshot,image->subresource_layouts,sizeof(snapshot));
    b.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,4,0,3};
    struct ps5vk_operation op={.type=PS5VK_IMAGE_BARRIER,.image_barrier=b};
    assert(ps5vk_image_linear_execute(device,&op)==VK_ERROR_DEVICE_LOST);
    assert(!memcmp(snapshot,image->subresource_layouts,sizeof(snapshot)));
    VkImageSubresourceRange r={VK_IMAGE_ASPECT_COLOR_BIT,3,VK_REMAINING_MIP_LEVELS,2,VK_REMAINING_ARRAY_LAYERS},out;
    assert(ps5vk_image_range_resolve(image,&r,&out)&&out.levelCount==1&&out.layerCount==1);
    r.levelCount=2;assert(!ps5vk_image_range_resolve(image,&r,&out));
    r.levelCount=1;r.baseArrayLayer=UINT32_MAX;assert(!ps5vk_image_range_resolve(image,&r,&out));
    r.baseArrayLayer=0;r.layerCount=0;assert(!ps5vk_image_range_resolve(image,&r,&out));
    /* Recording refuses overflowing ranges and access/stage mismatches. */
    VkCommandBuffer bad=begin();
    b.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,UINT32_MAX,2,0,1};
    vkCmdPipelineBarrier(bad,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    assert(vkEndCommandBuffer(bad)!=VK_SUCCESS);vkFreeCommandBuffers(device,pool,1,&bad);
    bad=begin();b.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(bad,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    assert(vkEndCommandBuffer(bad)!=VK_SUCCESS);vkFreeCommandBuffers(device,pool,1,&bad);
    /* Access support still follows usage, and a barrier cannot invent it. */
    struct VkImage_T transfer_only={.info={.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
        VK_IMAGE_USAGE_TRANSFER_DST_BIT}};
    assert(ps5vk_linear_layout_access(&transfer_only,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,1));
    assert(!ps5vk_linear_layout_access(&transfer_only,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,1));
    transfer_only.info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(!ps5vk_linear_layout_access(&transfer_only,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,1));
    /* The last range can become sampled without changing its neighbours. */
    b.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,2,1,1,1};
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    VkCommandBuffer sample=begin();
    vkCmdPipelineBarrier(sample,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,NULL,0,NULL,1,&b);submit_and_wait(sample);
    assert(ps5vk_image_layout_matches(image,2,1,1,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    assert(ps5vk_image_layout_matches(image,2,0,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
    VkImageSubresourceRange view_range={VK_IMAGE_ASPECT_COLOR_BIT,2,1,1,1};
    assert(ps5vk_image_range_layout_matches(image,&view_range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    view_range.layerCount=2;
    assert(!ps5vk_image_range_layout_matches(image,&view_range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    /* A sampled cell can return to transfer ownership without changing any
     * neighbouring mip/layer. Access scopes are checked against stages. */
    memcpy(snapshot,image->subresource_layouts,sizeof(snapshot));
    b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    bad=begin();
    vkCmdPipelineBarrier(bad,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    assert(vkEndCommandBuffer(bad)!=VK_SUCCESS);vkFreeCommandBuffers(device,pool,1,&bad);
    assert(!memcmp(snapshot,image->subresource_layouts,sizeof(snapshot)));
    VkCommandBuffer recycle=begin();
    vkCmdPipelineBarrier(recycle,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    submit_and_wait(recycle);vkFreeCommandBuffers(device,pool,1,&recycle);
    snapshot[2*3+1]=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    assert(!memcmp(snapshot,image->subresource_layouts,sizeof(snapshot)));
    assert(image->layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    /* REMAINING spans multiple mips and layers, with independent read/write
     * access bits in GENERAL. The untouched first layer/mip stays DST. */
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    b.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,1,
        VK_REMAINING_MIP_LEVELS,1,VK_REMAINING_ARRAY_LAYERS};
    recycle=begin();
    vkCmdPipelineBarrier(recycle,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    submit_and_wait(recycle);vkFreeCommandBuffers(device,pool,1,&recycle);
    for(unsigned m=0;m<4;++m)for(unsigned l=0;l<3;++l)
        assert(ps5vk_image_layout_matches(image,m,l,1,m>=1&&l>=1?
            VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
    assert(image->layout==VK_IMAGE_LAYOUT_MAX_ENUM);
    original_bc_image_copy_barriers();
    vkFreeCommandBuffers(device,pool,1,&first);vkFreeCommandBuffers(device,pool,1,&second);
    vkFreeCommandBuffers(device,pool,1,&sample);
    VkDeviceMemory memory=image->memory;vkDestroyImage(device,image,NULL);vkFreeMemory(device,memory,NULL);
    vkDestroyCommandPool(device,pool,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    return 0;
}

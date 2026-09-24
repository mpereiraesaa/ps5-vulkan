#include "vk_command.h"
#include "vk_image.h"
#include "vk_image_transfer.h"
#include "texture_copy.h"
#include "texture_format.h"
#include <stdint.h>
#define invalid ps5vk_command_invalidate

static int overlaps(uintptr_t a, VkDeviceSize a_size,
    uintptr_t b, VkDeviceSize b_size)
{
    return a < b ? a_size > b - a : b_size > a - b;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer source,
    VkBuffer destination, uint32_t count, const VkBufferCopy *regions)
{
    if (!c || c->state != PS5VK_RECORDING || c->render_pass || !count || !regions ||
        c->operation_count > PS5VK_MAX_OPERATIONS ||
        count > PS5VK_MAX_OPERATIONS - c->operation_count ||
        !ps5vk_buffer_usage(c->pool->device, source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !ps5vk_buffer_usage(c->pool->device, destination, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        invalid(c); return;
    }
    uintptr_t src[PS5VK_MAX_OPERATIONS], dst[PS5VK_MAX_OPERATIONS];
    VkDeviceSize sizes[PS5VK_MAX_OPERATIONS];
    for (uint32_t j = 0; j < count; ++j) {
        void *src_address, *dst_address;
        VkDeviceSize bytes;
        if (!regions[j].size ||
            ps5vk_buffer_span(c->pool->device, source, regions[j].srcOffset,
                regions[j].size, &src_address, &bytes) != VK_SUCCESS ||
            ps5vk_buffer_span(c->pool->device, destination, regions[j].dstOffset,
                regions[j].size, &dst_address, &bytes) != VK_SUCCESS) {
            invalid(c); return;
        }
        src[j] = (uintptr_t)src_address;
        dst[j] = (uintptr_t)dst_address;
        sizes[j] = regions[j].size;
    }
    for (uint32_t j = 0; j < count; ++j)
        for (uint32_t k = 0; k < count; ++k) {
            if (overlaps(src[j], sizes[j], dst[k], sizes[k]) ||
                (j != k && overlaps(dst[j], sizes[j], dst[k], sizes[k]))) {
                invalid(c); return;
            }
        }
    struct ps5vk_operation *ops = ps5vk_command_reserve_operations(c,
        PS5VK_COPY_BUFFER, PS5VK_OPERATION_OUTSIDE_RENDER_PASS, count);
    if (!ops) return;
    for (uint32_t j = 0; j < count; ++j) {
        ops[j].copy_source = source;
        ops[j].copy_destination = destination;
        ops[j].buffer_copy = regions[j];
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer destination,
    VkDeviceSize offset, VkDeviceSize size, const void *data)
{
    void *address;
    VkDeviceSize bytes;
    if (!c || c->state != PS5VK_RECORDING || c->render_pass || !data || !size ||
        size > 65536 || (offset & 3u) || (size & 3u) ||
        !ps5vk_buffer_usage(c->pool->device, destination, VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        ps5vk_buffer_span(c->pool->device, destination, offset, size,
            &address, &bytes) != VK_SUCCESS) {
        invalid(c); return;
    }
    struct ps5vk_operation *op = ps5vk_command_reserve_operation_with_payload(c,
        PS5VK_UPDATE_BUFFER, PS5VK_OPERATION_OUTSIDE_RENDER_PASS, data, (size_t)size);
    if (!op) return;
    op->copy_destination = destination;
    op->buffer_offset = offset;
    op->buffer_size = size;
}

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer c, VkBuffer destination,
    VkDeviceSize offset, VkDeviceSize size, uint32_t data)
{
    void *address;
    VkDeviceSize bytes;
    if (!c || c->state != PS5VK_RECORDING || c->render_pass || (offset & 3u) ||
        (size != VK_WHOLE_SIZE && (!size || (size & 3u))) ||
        !ps5vk_buffer_usage(c->pool->device, destination, VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        ps5vk_buffer_span(c->pool->device, destination, offset, size,
            &address, &bytes) != VK_SUCCESS) {
        invalid(c); return;
    }
    if (size == VK_WHOLE_SIZE) bytes &= ~(VkDeviceSize)3;
    if (!bytes) return;
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(c,
        PS5VK_FILL_BUFFER, PS5VK_OPERATION_OUTSIDE_RENDER_PASS, 1);
    if (!op) return;
    op->copy_destination = destination;
    op->buffer_offset = offset;
    op->buffer_size = bytes;
    op->fill_data = data;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer c,VkBuffer source,VkImage image,
    VkImageLayout layout,uint32_t count,const VkBufferImageCopy *regions)
{
    if(!c || c->state!=PS5VK_RECORDING || c->render_pass || !count || !regions ||
        count>PS5VK_MAX_OPERATIONS-c->operation_count || !image || image->device!=c->pool->device ||
        (layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && layout!=VK_IMAGE_LAYOUT_GENERAL)) {invalid(c);return;}
    VkDevice d=c->pool->device;
    /* Two destinations are real: the sampled role uploaded by the GPU prelude,
     * and the host-visible padded-linear transfer role. */
    const VkImageUsageFlags usage=image->info.usage;
    const int sampled_upload=(usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        ps5vk_texture_format_sampled_image(image->info.format) &&
        !(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    /* The transfer role and the colour-attachment shape that declares a
     * transfer destination share the padded linear upload, which is why the
     * pinned upstream draw tests can create their colour target with a
     * transfer destination and still upload into it. */
    const int linear_upload=(ps5vk_pure_transfer_image(image) ||
        ps5vk_colour_transfer_image(image)) &&
        (usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if(!ps5vk_buffer_usage(d,source,VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !ps5vk_texture_format_sampled_image(image->info.format) ||
        (!sampled_upload && !linear_upload)) {invalid(c);return;}
    void *src,*dst;VkDeviceSize src_bytes,dst_bytes;
    if(ps5vk_buffer_span(d,source,0,VK_WHOLE_SIZE,&src,&src_bytes)!=VK_SUCCESS ||
        ps5vk_image_span(d,image,&dst,&dst_bytes)!=VK_SUCCESS) {invalid(c);return;}
    /* Validate the whole call before appending any operation. Region structs
     * are copied; later application mutations cannot change recorded work. */
    for(uint32_t i=0;i<count;++i) {
        struct ps5vk_texture_copy plan;
        /* The padded-linear transfer role maps the same region as the sampled
         * upload path, so the shared planner is the record-time gate for both;
         * ps5vk_image_transfer.c keeps a queue-group-local copy of the same
         * arithmetic, and tests/test_image_copy_clear.c asserts they agree. */
        if(ps5vk_texture_copy_plan_for_image(image,src_bytes,dst_bytes,
            &regions[i],&plan)!=VK_SUCCESS) {invalid(c);return;}
    }
    struct ps5vk_operation *ops=ps5vk_command_reserve_operations(c,PS5VK_COPY_BUFFER_IMAGE,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,count);
    if(!ops)return;
    for(uint32_t i=0;i<count;++i) {
        ops[i].copy_source=source;ops[i].copy_image=image;
        ops[i].copy_layout=layout;ops[i].copy_region=regions[i];
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage image,
    VkImageLayout layout,VkBuffer destination,uint32_t count,const VkBufferImageCopy *regions)
{
    if(!c || c->state!=PS5VK_RECORDING || c->render_pass || count!=1 || !regions ||
        c->operation_count==PS5VK_MAX_OPERATIONS || !image || image->device!=c->pool->device ||
        (layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
         !(layout==VK_IMAGE_LAYOUT_GENERAL && ps5vk_storage_image(image)))) {invalid(c);return;}
    VkDevice d=c->pool->device;
    /* Block-compressed images use the block-padded linear layout; keep their
     * readback away from the generic RGBA8 texel-row planner. */
    if(ps5vk_bc_linear_image(image) &&
       (image->info.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        void *src,*dst;VkDeviceSize src_bytes,dst_bytes;
        if(!ps5vk_buffer_usage(d,destination,VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
           ps5vk_image_span(d,image,&src,&src_bytes)!=VK_SUCCESS ||
           ps5vk_buffer_span(d,destination,0,VK_WHOLE_SIZE,&dst,&dst_bytes)!=VK_SUCCESS) {invalid(c);return;}
        struct ps5vk_texture_copy plan;
        if(ps5vk_texture_copy_plan_for_image(image,dst_bytes,src_bytes,
            &regions[0],&plan)!=VK_SUCCESS) {invalid(c);return;}
        struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_COPY_IMAGE_BUFFER,
            PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
        if(!op)return;
        op->copy_destination=destination;op->copy_image=image;
        op->copy_layout=layout;op->copy_region=regions[0];
        return;
    }
    /* The pure transfer role is host-visible memory, so its readback is a
     * frontend copy over the same padded layout. It needs no graphics backend,
     * and it accepts the tight row description the original CTS oracle uses. */
    if((ps5vk_pure_transfer_image(image) || ps5vk_storage_image(image)) &&
       (image->info.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        void *src,*dst;VkDeviceSize src_bytes,dst_bytes;
        if(!ps5vk_buffer_usage(d,destination,VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
           ps5vk_image_span(d,image,&src,&src_bytes)!=VK_SUCCESS ||
           ps5vk_buffer_span(d,destination,0,VK_WHOLE_SIZE,&dst,&dst_bytes)!=VK_SUCCESS) {invalid(c);return;}
        struct ps5vk_texture_copy plan;
        if(ps5vk_texture_copy_plan_for_format(image->info.format,image->info.extent.width,image->info.extent.height,
            dst_bytes,src_bytes,&regions[0],&plan)!=VK_SUCCESS) {invalid(c);return;}
        struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_COPY_IMAGE_BUFFER,
            PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
        if(!op)return;
        op->copy_destination=destination;op->copy_image=image;
        op->copy_layout=layout;op->copy_region=regions[0];
        return;
    }
    /* The depth readback: the same whole-surface shape as the colour one, over
     * the DEPTH aspect of a D32 attachment that declares the transfer source
     * role. It reaches the graphics backend like the colour readback does,
     * because the bytes are tiled and only the GPU's completion makes them
     * readable; the detile itself is SW_64K_Z_X (src/depth_detile.c). The
     * pinned dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat* family reads its
     * depth attachment back exactly this way (vktDrawDepthClampTests.cpp:554). */
    if(ps5vk_depth_readback_image(image)) {
        const VkBufferImageCopy *dr=&regions[0];
        void *dsrc,*ddst;VkDeviceSize dsrc_bytes,ddst_bytes;
        const uint64_t dpixels=(uint64_t)image->info.extent.width*image->info.extent.height;
        if(!d->graphics_enabled ||
           !ps5vk_buffer_usage(d,destination,VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
           dr->bufferOffset ||
           (dr->bufferRowLength && dr->bufferRowLength!=image->info.extent.width) ||
           (dr->bufferImageHeight && dr->bufferImageHeight!=image->info.extent.height) ||
           dr->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_DEPTH_BIT ||
           dr->imageSubresource.mipLevel || dr->imageSubresource.baseArrayLayer ||
           dr->imageSubresource.layerCount!=1 ||
           dr->imageOffset.x || dr->imageOffset.y || dr->imageOffset.z ||
           dr->imageExtent.width!=image->info.extent.width ||
           dr->imageExtent.height!=image->info.extent.height || dr->imageExtent.depth!=1 ||
           !dpixels || dpixels>UINT64_MAX/4 ||
           ps5vk_image_span(d,image,&dsrc,&dsrc_bytes)!=VK_SUCCESS ||
           ps5vk_buffer_span(d,destination,0,VK_WHOLE_SIZE,&ddst,&ddst_bytes)!=VK_SUCCESS ||
           ddst_bytes<dpixels*4 ||
           overlaps((uintptr_t)dsrc,dsrc_bytes,(uintptr_t)ddst,dpixels*4)) {invalid(c);return;}
        struct ps5vk_operation *dop=ps5vk_command_reserve_operations(c,PS5VK_COPY_IMAGE_BUFFER,
            PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
        if(!dop)return;
        dop->copy_destination=destination;dop->copy_image=image;
        dop->copy_layout=layout;dop->copy_region=*dr;
        return;
    }
    const int array_color=ps5vk_array_color_image(image);
    if(!d->graphics_enabled || !ps5vk_buffer_usage(d,destination,VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        (image->info.format!=VK_FORMAT_R8G8B8A8_UNORM &&
         !ps5vk_color_target_integer_served(image->info.format)) || image->info.mipLevels!=1 ||
        (!array_color && image->info.arrayLayers!=1) || image->info.extent.depth!=1 ||
        (image->info.usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))!=
            (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        /* A transfer destination does not stop a colour attachment being read
         * back: the pinned upstream draw helper clears such a target outside
         * the render pass and then reads the rendered result from the same
         * image (vkImageUtil.cpp clearColorImage, Image::read). The depth
         * stencil role stays out. A sampled role is admitted only for the
         * colour attachment whose format publishes it, which is the pinned
         * render-pass module's own readback target
         * (vktRenderPassTests.cpp:5307): the copy reads the attachment's
         * bytes, never through the sampled view, and a build that does not
         * serve that role keeps refusing the combination. The transfer
         * destination is admitted only for the colour-attachment shape that
         * declares it. */
        (!array_color &&
         (image->info.usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ||
        (!array_color && (image->info.usage&VK_IMAGE_USAGE_SAMPLED_BIT) &&
         !ps5vk_colour_transfer_image(image)) ||
        (!array_color && (image->info.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
         !ps5vk_colour_transfer_image(image))) {invalid(c);return;}
    const VkBufferImageCopy *r=&regions[0];
    const uint64_t plane=(uint64_t)image->info.extent.width*image->info.extent.height;
    if(!image->info.arrayLayers || plane>UINT64_MAX/4/image->info.arrayLayers){invalid(c);return;}
    const uint64_t pixels=plane*image->info.arrayLayers;
    void *src,*dst;VkDeviceSize src_bytes,dst_bytes;
    if(r->bufferOffset || (r->bufferRowLength && r->bufferRowLength!=image->info.extent.width) ||
        (r->bufferImageHeight && r->bufferImageHeight!=image->info.extent.height) ||
        r->imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
        r->imageSubresource.mipLevel || r->imageSubresource.baseArrayLayer ||
        r->imageSubresource.layerCount!=image->info.arrayLayers || r->imageOffset.x || r->imageOffset.y ||
        r->imageOffset.z || r->imageExtent.width!=image->info.extent.width ||
        r->imageExtent.height!=image->info.extent.height || r->imageExtent.depth!=1 ||
        pixels>UINT64_MAX/4 ||
        ps5vk_image_span(d,image,&src,&src_bytes)!=VK_SUCCESS ||
        ps5vk_buffer_span(d,destination,0,VK_WHOLE_SIZE,&dst,&dst_bytes)!=VK_SUCCESS ||
        dst_bytes<pixels*4 || overlaps((uintptr_t)src,src_bytes,(uintptr_t)dst,pixels*4)) {invalid(c);return;}
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_COPY_IMAGE_BUFFER,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
    if(!op)return;
    op->copy_destination=destination;op->copy_image=image;
    op->copy_layout=layout;op->copy_region=*r;
}

/*
 * Vulkan 1.0 image copy and colour image clear.
 *
 * Implemented role: RGBA8 with TRANSFER_SRC|TRANSFER_DST usage, backed by the
 * padded linear layout (ps5vk_texture_layout). Copy and clear are row copies or
 * row memsets on host-visible direct memory, executed in submission order at
 * the head of the queue chain. Every write flushes the exact bound destination
 * allocation range before a following GPU segment.
 */
#include "vk_image_transfer.h"
#include "vk_image.h"
#include "color_clear.h"
#include <stdint.h>
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN

/* Return non-zero for overlap or an address-range representation failure.  The
 * implementation conservatively rejects distinct Vulkan resources backed by
 * overlapping allocation spans; its row copies may therefore use memcpy. */
static int spans_overlap(const void *a, VkDeviceSize a_bytes,
    const void *b, VkDeviceSize b_bytes)
{
    const uintptr_t a0 = (uintptr_t)a, b0 = (uintptr_t)b;
    if (!a || !b || !a_bytes || !b_bytes ||
        a_bytes > UINTPTR_MAX - a0 || b_bytes > UINTPTR_MAX - b0)
        return 1;
    return a0 < b0 + (uintptr_t)b_bytes && b0 < a0 + (uintptr_t)a_bytes;
}

/* Padded linear layout of the advertised transfer role. Kept local so this
 * module does not depend on the graphics source group; tests/test_image_copy_clear.c
 * asserts it equals ps5vk_texture_layout() so the two formulas cannot drift. */
static int transfer_layout(uint32_t width, uint32_t height, uint32_t *pitch, VkDeviceSize *bytes)
{
    if (!pitch || !bytes || !width || !height || width > 16384 || height > 16384) return -1;
    uint32_t row = (width * 4u + 255u) & ~255u;
    *pitch = row;
    *bytes = (VkDeviceSize)row * height;
    return 0;
}

/* One base-level RGBA8 2D region mapped between a buffer row description and
 * the padded image row layout, in either direction. This deliberately mirrors
 * ps5vk_texture_copy_plan (graphics source group) instead of linking it, so the
 * queue-only fixtures stay linkable; tests/test_image_copy_clear.c asserts both
 * planners agree over a region matrix. */
struct linear_copy {
    VkDeviceSize buffer_offset, image_offset, buffer_pitch, image_pitch;
    uint32_t row_bytes, rows;
};

static int linear_region_plan(uint32_t width, uint32_t height,
    VkDeviceSize buffer_bytes, VkDeviceSize image_bytes,
    const VkBufferImageCopy *r, struct linear_copy *out)
{
    uint32_t image_pitch = 0;
    VkDeviceSize layout_bytes = 0;
    if (!r || !out || transfer_layout(width, height, &image_pitch, &layout_bytes)) return -1;
    if (image_bytes < layout_bytes ||
        r->imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
        r->imageSubresource.mipLevel || r->imageSubresource.baseArrayLayer ||
        r->imageSubresource.layerCount != 1 ||
        r->imageOffset.x < 0 || r->imageOffset.y < 0 || r->imageOffset.z ||
        r->imageExtent.depth != 1 || !r->imageExtent.width || !r->imageExtent.height ||
        (r->bufferOffset & 3u)) return -1;
    uint32_t x = (uint32_t)r->imageOffset.x, y = (uint32_t)r->imageOffset.y;
    if (x > width || y > height ||
        r->imageExtent.width > width - x || r->imageExtent.height > height - y ||
        (r->bufferRowLength && r->bufferRowLength < r->imageExtent.width) ||
        (r->bufferImageHeight && r->bufferImageHeight < r->imageExtent.height)) return -1;
    uint64_t buffer_pitch = 4ull * (r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width);
    uint64_t row_bytes = 4ull * r->imageExtent.width, rows_before = r->imageExtent.height - 1;
    if (r->bufferOffset > buffer_bytes || row_bytes > buffer_bytes - r->bufferOffset ||
        rows_before > (buffer_bytes - r->bufferOffset - row_bytes) / buffer_pitch) return -1;
    out->buffer_offset = r->bufferOffset;
    out->image_offset = (VkDeviceSize)y * image_pitch + 4ull * x;
    out->buffer_pitch = buffer_pitch;
    out->image_pitch = image_pitch;
    out->row_bytes = (uint32_t)row_bytes;
    out->rows = r->imageExtent.height;
    return 0;
}

VkBool32 ps5vk_image_transfer_operation(enum ps5vk_operation_type type)
{
    return type == PS5VK_COPY_IMAGE || type == PS5VK_CLEAR_COLOR_IMAGE;
}

/* Direction roles of the advertised pure transfer role. A copy source needs the
 * transfer-source bit; a copy or clear destination needs the transfer-destination
 * bit. Both are the same padded linear layout. */
static int transfer_role_source(VkImage image)
{
    return ps5vk_pure_transfer_image(image) &&
        (image->info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}
static int transfer_role_destination(VkImage image)
{
    /* The transfer-only role and the colour-attachment readback shape that also
     * declares a transfer destination: both are padded linear memory, and the
     * clear below fills either of them the same way. */
    return (ps5vk_pure_transfer_image(image) || ps5vk_colour_transfer_image(image)) &&
        (image->info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

/* Frontend work of the pure transfer role: image copies and clears, image <->
 * buffer transfers, and layout bookkeeping. The tiled colour-attachment role
 * keeps its GPU prelude/postlude path, and the sampled role keeps its upload
 * prelude, so neither is claimed here. */
VkBool32 ps5vk_image_linear_operation(const struct ps5vk_operation *op)
{
    if (!op) return VK_FALSE;
    switch (op->type) {
    case PS5VK_COPY_BUFFER_IMAGE:
        /* An upload into the colour-attachment shape that declares a transfer
         * destination is frontend work for the same reason the transfer role
         * is: linear memory, no graphics backend involvement. */
        return ps5vk_pure_transfer_image(op->copy_image) ||
            ps5vk_colour_transfer_image(op->copy_image);
    case PS5VK_COPY_IMAGE_BUFFER:
        return ps5vk_pure_transfer_image(op->copy_image);
    case PS5VK_IMAGE_BARRIER:
        return ps5vk_pure_transfer_image(op->image_barrier.image);
    default:
        return VK_FALSE;
    }
}

static int layout_is_transfer_source(VkImageLayout layout)
{ return layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || layout == VK_IMAGE_LAYOUT_GENERAL; }

static int layout_is_transfer_destination(VkImageLayout layout)
{ return layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL || layout == VK_IMAGE_LAYOUT_GENERAL; }


VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer c, VkImage source,
    VkImageLayout source_layout, VkImage destination, VkImageLayout destination_layout,
    uint32_t region_count, const VkImageCopy *regions)
{
    if (!c || c->state != PS5VK_RECORDING || !region_count || !regions) {
        ps5vk_command_invalidate(c);
        return;
    }
    VkDevice d = c->pool->device;
    void *source_address = NULL, *destination_address = NULL;
    VkDeviceSize source_bytes = 0, destination_bytes = 0;
    if (!transfer_role_source(source) || !transfer_role_destination(destination) ||
        !layout_is_transfer_source(source_layout) ||
        !layout_is_transfer_destination(destination_layout) ||
        source == destination ||
        ps5vk_image_span(d, source, &source_address, &source_bytes) != VK_SUCCESS ||
        ps5vk_image_span(d, destination, &destination_address, &destination_bytes) != VK_SUCCESS ||
        spans_overlap(source_address, source_bytes, destination_address, destination_bytes)) {
        ps5vk_command_invalidate(c);
        return;
    }
    /* Validate every region before reserving any operation. */
    for (uint32_t i = 0; i < region_count; ++i) {
        const VkImageCopy *r = &regions[i];
        if (r->srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            r->dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            r->srcSubresource.mipLevel || r->dstSubresource.mipLevel ||
            r->srcSubresource.baseArrayLayer || r->dstSubresource.baseArrayLayer ||
            r->srcSubresource.layerCount != 1 || r->dstSubresource.layerCount != 1 ||
            !r->extent.width || !r->extent.height || r->extent.depth != 1 ||
            r->srcOffset.x < 0 || r->srcOffset.y < 0 || r->srcOffset.z ||
            r->dstOffset.x < 0 || r->dstOffset.y < 0 || r->dstOffset.z ||
            r->extent.width > source->info.extent.width ||
            r->extent.height > source->info.extent.height ||
            r->extent.width > destination->info.extent.width ||
            r->extent.height > destination->info.extent.height ||
            (uint32_t)r->srcOffset.x > source->info.extent.width - r->extent.width ||
            (uint32_t)r->srcOffset.y > source->info.extent.height - r->extent.height ||
            (uint32_t)r->dstOffset.x > destination->info.extent.width - r->extent.width ||
            (uint32_t)r->dstOffset.y > destination->info.extent.height - r->extent.height) {
            ps5vk_command_invalidate(c);
            return;
        }
    }
    struct ps5vk_operation *op = ps5vk_command_reserve_operation_with_payload(c,
        PS5VK_COPY_IMAGE, PS5VK_OPERATION_OUTSIDE_RENDER_PASS, regions,
        region_count * sizeof(*regions));
    if (!op) return;
    op->image_source = source;
    op->image_destination = destination;
    op->image_source_layout = source_layout;
    op->image_destination_layout = destination_layout;
    op->image_region_count = region_count;
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer c, VkImage image,
    VkImageLayout image_layout, const VkClearColorValue *color,
    uint32_t range_count, const VkImageSubresourceRange *ranges)
{
    if (!c || c->state != PS5VK_RECORDING || !color || !range_count || !ranges) {
        ps5vk_command_invalidate(c);
        return;
    }
    VkDevice d = c->pool->device;
    void *address = NULL;
    VkDeviceSize bytes = 0;
    if (!transfer_role_destination(image) || !layout_is_transfer_destination(image_layout) ||
        ps5vk_image_span(d, image, &address, &bytes) != VK_SUCCESS) {
        ps5vk_command_invalidate(c);
        return;
    }
    /* RGBA8 UNORM clear values must be finite [0,1] floats; reuse the render
     * pass clear conversion so both paths agree on the byte encoding. */
    uint32_t word = 0;
    if (!ps5vk_color_clear_rgba8(color->float32, &word)) {
        ps5vk_command_invalidate(c);
        return;
    }
    for (uint32_t i = 0; i < range_count; ++i) {
        const VkImageSubresourceRange *r = &ranges[i];
        if (r->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || r->baseMipLevel ||
            (r->levelCount != 1 && r->levelCount != VK_REMAINING_MIP_LEVELS) ||
            r->baseArrayLayer ||
            (r->layerCount != 1 && r->layerCount != VK_REMAINING_ARRAY_LAYERS)) {
            ps5vk_command_invalidate(c);
            return;
        }
    }
    struct ps5vk_operation *op = ps5vk_command_reserve_operation_with_payload(c,
        PS5VK_CLEAR_COLOR_IMAGE, PS5VK_OPERATION_OUTSIDE_RENDER_PASS, ranges,
        range_count * sizeof(*ranges));
    if (!op) return;
    op->image_destination = image;
    op->image_destination_layout = image_layout;
    op->clear_word = word;
    op->image_region_count = range_count;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer c, VkImage source,
    VkImageLayout source_layout, VkImage destination, VkImageLayout destination_layout,
    uint32_t region_count, const VkImageBlit *regions, VkFilter filter)
{
    (void)source; (void)source_layout; (void)destination; (void)destination_layout;
    (void)region_count; (void)regions; (void)filter;
    /* Scaling/filtering has no proven GFX1013 packet or bounded CPU contract. */
    ps5vk_command_invalidate(c);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer c, VkImage source,
    VkImageLayout source_layout, VkImage destination, VkImageLayout destination_layout,
    uint32_t region_count, const VkImageResolve *regions)
{
    (void)source; (void)source_layout; (void)destination; (void)destination_layout;
    (void)region_count; (void)regions;
    /* The advertised image roles are single-sampled; no resolve is supported. */
    ps5vk_command_invalidate(c);
}

/* Whole-subresource depth clear.
 *
 * The surface is 64KB_Z_X tiled and this codebase deliberately does not claim
 * the pipe XOR pixel equations (src/depth_layout.h). It does not need them
 * here: every texel of the one-sample D32 surface receives the SAME 32-bit
 * word, so writing that word over the entire allocation is tiling-invariant
 * and produces exactly the image a per-pixel clear would. The native emitter
 * is the uniform-DWORD DMA fill the render pass already uses for its depth
 * load-op clear on hardware (ps5vk_dma_fill, src/texture_dma.c).
 *
 * Consequently only the whole subresource is accepted. A partial range, a
 * rectangle, a stencil aspect, a combined depth/stencil format, a second mip
 * or layer and any multisample image stay fail-closed, because each of those
 * does need the pixel addressing this driver has not proven.
 *
 * value->stencil is IGNORED, not rejected. Vulkan uses that member only for a
 * range whose aspect mask includes VK_IMAGE_ASPECT_STENCIL_BIT, and the only
 * range accepted here is depth-only, so a nonzero stencil is a valid call that
 * clears depth alone. The pinned upstream CTS relies on exactly that: the
 * depth/stencil copy tests clear a D32_SFLOAT image with
 * makeClearValueDepthStencil(0.1f, 0x10). Rejecting it would refuse a
 * conformant call and fail those cases. */
VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage image,
    VkImageLayout image_layout, const VkClearDepthStencilValue *value,
    uint32_t range_count, const VkImageSubresourceRange *ranges)
{
    if (!c || c->state != PS5VK_RECORDING || c->render_pass || !value ||
        !range_count || !ranges) { ps5vk_command_invalidate(c); return; }
    VkDevice d = c->pool->device;
    void *address = NULL;
    VkDeviceSize bytes = 0;
    uint32_t word = 0;
    if (!image || image->device != d || !ps5vk_depth_clear_image(image) ||
        !layout_is_transfer_destination(image_layout) ||
        !ps5vk_depth_clear_word(value->depth, &word) ||
        ps5vk_image_span(d, image, &address, &bytes) != VK_SUCCESS) {
        ps5vk_command_invalidate(c);
        return;
    }
    for (uint32_t i = 0; i < range_count; ++i) {
        const VkImageSubresourceRange *r = &ranges[i];
        if (r->aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT || r->baseMipLevel ||
            (r->levelCount != 1 && r->levelCount != VK_REMAINING_MIP_LEVELS) ||
            r->baseArrayLayer ||
            (r->layerCount != 1 && r->layerCount != VK_REMAINING_ARRAY_LAYERS)) {
            ps5vk_command_invalidate(c);
            return;
        }
    }
    struct ps5vk_operation *op = ps5vk_command_reserve_operation_with_payload(c,
        PS5VK_CLEAR_DEPTH_STENCIL_IMAGE, PS5VK_OPERATION_OUTSIDE_RENDER_PASS,
        ranges, range_count * sizeof(*ranges));
    if (!op) return;
    op->image_destination = image;
    op->image_destination_layout = image_layout;
    op->clear_word = word;
    op->image_region_count = range_count;
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer c,
    uint32_t attachment_count, const VkClearAttachment *attachments,
    uint32_t rect_count, const VkClearRect *rects)
{
    (void)attachments; (void)rects;
    if (!c || c->state != PS5VK_RECORDING) { ps5vk_command_invalidate(c); return; }
    if (!attachment_count || !attachments || !rect_count || !rects || !c->render_pass) {
        ps5vk_command_invalidate(c);
        return;
    }
    /* Fail closed: a clear recorded inside a render pass needs a DCB clear path
     * (or a tiled writer) that this profile does not implement yet. */
    ps5vk_command_invalidate(c);
}

VkResult ps5vk_image_transfer_execute(VkDevice d, const struct ps5vk_operation *op)
{
    if (!d || !op) return VK_ERROR_DEVICE_LOST;
    if (ps5vk_image_transfer_validate(d, op) != VK_SUCCESS) return VK_ERROR_DEVICE_LOST;
    void *source_address = NULL, *destination_address = NULL;
    VkDeviceSize source_bytes = 0, destination_bytes = 0;
    if (op->type == PS5VK_COPY_IMAGE) {
        const VkImageCopy *regions = (const VkImageCopy *)op->owned_payload;
        if (!regions || !op->image_region_count ||
            op->owned_payload_size != (size_t)op->image_region_count * sizeof(*regions) ||
            op->image_source->layout != op->image_source_layout ||
            op->image_destination->layout != op->image_destination_layout)
            return VK_ERROR_DEVICE_LOST;
        if (ps5vk_image_span(d, op->image_source, &source_address, &source_bytes) != VK_SUCCESS ||
            ps5vk_image_span(d, op->image_destination, &destination_address, &destination_bytes) != VK_SUCCESS)
            return VK_ERROR_DEVICE_LOST;
        if (spans_overlap(source_address, source_bytes, destination_address, destination_bytes))
            return VK_ERROR_DEVICE_LOST;
        uint32_t source_pitch = 0, destination_pitch = 0;
        VkDeviceSize source_size = 0, destination_size = 0;
        if (transfer_layout(op->image_source->info.extent.width,
                op->image_source->info.extent.height, &source_pitch, &source_size) ||
            transfer_layout(op->image_destination->info.extent.width,
                op->image_destination->info.extent.height, &destination_pitch, &destination_size))
            return VK_ERROR_DEVICE_LOST;
        for (uint32_t i = 0; i < op->image_region_count; ++i) {
            const VkImageCopy *r = &regions[i];
            const size_t row_bytes = (size_t)r->extent.width * 4u;
            for (uint32_t y = 0; y < r->extent.height; ++y) {
                const size_t source_row = (size_t)(r->srcOffset.y + (int32_t)y) * source_pitch +
                    (size_t)r->srcOffset.x * 4u;
                const size_t destination_row = (size_t)(r->dstOffset.y + (int32_t)y) * destination_pitch +
                    (size_t)r->dstOffset.x * 4u;
                if (source_row + row_bytes > source_bytes || destination_row + row_bytes > destination_bytes)
                    return VK_ERROR_DEVICE_LOST;
                memcpy((unsigned char *)destination_address + destination_row,
                       (const unsigned char *)source_address + source_row, row_bytes);
            }
            VkDeviceSize flush_offset = (VkDeviceSize)(size_t)r->dstOffset.y * destination_pitch;
            VkDeviceSize flush_size = (VkDeviceSize)r->extent.height * destination_pitch;
            if (flush_offset + flush_size > destination_bytes) return VK_ERROR_DEVICE_LOST;
            if (ps5vk_image_flush_range(d, op->image_destination, flush_offset, flush_size) != VK_SUCCESS)
                return VK_ERROR_DEVICE_LOST;
        }
        return VK_SUCCESS;
    }
    if (op->type == PS5VK_CLEAR_COLOR_IMAGE) {
        if (op->image_destination->layout != op->image_destination_layout ||
            ps5vk_image_span(d, op->image_destination, &destination_address, &destination_bytes) != VK_SUCCESS)
            return VK_ERROR_DEVICE_LOST;
        uint32_t pitch = 0;
        VkDeviceSize layout_bytes = 0;
        if (transfer_layout(op->image_destination->info.extent.width,
                op->image_destination->info.extent.height, &pitch, &layout_bytes))
            return VK_ERROR_DEVICE_LOST;
        const size_t row_bytes = (size_t)op->image_destination->info.extent.width * 4u;
        for (uint32_t y = 0; y < op->image_destination->info.extent.height; ++y) {
            size_t row = (size_t)y * pitch;
            if (row + row_bytes > destination_bytes) return VK_ERROR_DEVICE_LOST;
            for (size_t x = 0; x + 4 <= row_bytes; x += 4)
                memcpy((unsigned char *)destination_address + row + x, &op->clear_word, 4);
        }
        return ps5vk_image_flush_range(d, op->image_destination, 0, layout_bytes) == VK_SUCCESS ?
            VK_SUCCESS : VK_ERROR_DEVICE_LOST;
    }
    return VK_ERROR_DEVICE_LOST;
}

VkResult ps5vk_image_transfer_validate(VkDevice d, const struct ps5vk_operation *op)
{
    if (!d || !op || !ps5vk_image_transfer_operation(op->type)) return INVALID;
    void *address = NULL;
    VkDeviceSize bytes = 0;
    if (op->type == PS5VK_COPY_IMAGE) {
        const VkImageCopy *regions = (const VkImageCopy *)op->owned_payload;
        void *source_address = NULL, *destination_address = NULL;
        VkDeviceSize source_bytes = 0, destination_bytes = 0;
        if (!op->image_region_count || !regions ||
            op->owned_payload_size != (size_t)op->image_region_count * sizeof(*regions) ||
            !transfer_role_source(op->image_source) ||
            !transfer_role_destination(op->image_destination) ||
            op->image_source == op->image_destination ||
            !layout_is_transfer_source(op->image_source_layout) ||
            !layout_is_transfer_destination(op->image_destination_layout) ||
            ps5vk_image_span(d, op->image_source, &source_address, &source_bytes) != VK_SUCCESS ||
            ps5vk_image_span(d, op->image_destination, &destination_address, &destination_bytes) != VK_SUCCESS ||
            spans_overlap(source_address, source_bytes, destination_address, destination_bytes))
            return INVALID;
        for (uint32_t i = 0; i < op->image_region_count; ++i) {
            const VkImageCopy *r = &regions[i];
            if (r->srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                r->dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                r->srcSubresource.mipLevel || r->dstSubresource.mipLevel ||
                r->srcSubresource.baseArrayLayer || r->dstSubresource.baseArrayLayer ||
                r->srcSubresource.layerCount != 1 || r->dstSubresource.layerCount != 1 ||
                !r->extent.width || !r->extent.height || r->extent.depth != 1 ||
                r->srcOffset.x < 0 || r->srcOffset.y < 0 || r->srcOffset.z ||
                r->dstOffset.x < 0 || r->dstOffset.y < 0 || r->dstOffset.z ||
                r->extent.width > op->image_source->info.extent.width ||
                r->extent.height > op->image_source->info.extent.height ||
                r->extent.width > op->image_destination->info.extent.width ||
                r->extent.height > op->image_destination->info.extent.height ||
                (uint32_t)r->srcOffset.x > op->image_source->info.extent.width - r->extent.width ||
                (uint32_t)r->srcOffset.y > op->image_source->info.extent.height - r->extent.height ||
                (uint32_t)r->dstOffset.x > op->image_destination->info.extent.width - r->extent.width ||
                (uint32_t)r->dstOffset.y > op->image_destination->info.extent.height - r->extent.height)
                return INVALID;
        }
        return VK_SUCCESS;
    }
    const VkImageSubresourceRange *ranges =
        (const VkImageSubresourceRange *)op->owned_payload;
    if (!op->image_region_count || !ranges ||
        op->owned_payload_size != (size_t)op->image_region_count * sizeof(*ranges) ||
        !transfer_role_destination(op->image_destination) ||
        !layout_is_transfer_destination(op->image_destination_layout) ||
        ps5vk_image_span(d, op->image_destination, &address, &bytes) != VK_SUCCESS)
        return INVALID;
    for (uint32_t i = 0; i < op->image_region_count; ++i) {
        const VkImageSubresourceRange *r = &ranges[i];
        if (r->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || r->baseMipLevel ||
            (r->levelCount != 1 && r->levelCount != VK_REMAINING_MIP_LEVELS) ||
            r->baseArrayLayer ||
            (r->layerCount != 1 && r->layerCount != VK_REMAINING_ARRAY_LAYERS))
            return INVALID;
    }
    return VK_SUCCESS;
}

/* Static re-validation of an image <-> buffer transfer or a layout barrier on
 * the pure transfer role. Recorded-order state (the image's current layout) is
 * deliberately checked at execution instead: a barrier recorded earlier in the
 * same command buffer has not run yet when the queue submit is validated. */
VkResult ps5vk_image_linear_validate(VkDevice d, const struct ps5vk_operation *op)
{
    if (!d || !op || !ps5vk_image_linear_operation(op)) return INVALID;
    void *address = NULL;
    VkDeviceSize bytes = 0;
    if (op->type == PS5VK_IMAGE_BARRIER) {
        const VkImageMemoryBarrier *b = &op->image_barrier;
        if (!layout_is_transfer_source(b->oldLayout) &&
            b->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
            !layout_is_transfer_destination(b->oldLayout)) return INVALID;
        if (!layout_is_transfer_source(b->newLayout) &&
            !layout_is_transfer_destination(b->newLayout)) return INVALID;
        /* Only transfer dependencies can order a transfer-only image. */
        if ((b->srcAccessMask | b->dstAccessMask) &
            ~(VkAccessFlags)(VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT))
            return INVALID;
        if (ps5vk_image_span(d, b->image, &address, &bytes) != VK_SUCCESS) return INVALID;
        return VK_SUCCESS;
    }
    const VkBuffer buffer = op->type == PS5VK_COPY_BUFFER_IMAGE ?
        op->copy_source : op->copy_destination;
    if (!ps5vk_pure_transfer_image(op->copy_image) ||
        !ps5vk_buffer_usage(d, buffer, op->type == PS5VK_COPY_BUFFER_IMAGE ?
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        (op->type == PS5VK_COPY_BUFFER_IMAGE ?
            !transfer_role_destination(op->copy_image) : !transfer_role_source(op->copy_image)) ||
        (op->type == PS5VK_COPY_BUFFER_IMAGE ?
            !layout_is_transfer_destination(op->copy_layout) :
            !layout_is_transfer_source(op->copy_layout)) ||
        ps5vk_image_span(d, op->copy_image, &address, &bytes) != VK_SUCCESS ||
        ps5vk_buffer_span(d, buffer, 0, VK_WHOLE_SIZE, &address, &bytes) != VK_SUCCESS)
        return INVALID;
    /* The region mapping is role-independent: both directions describe one
     * RGBA8 base-level 2D area between a tightly described buffer row and the
     * padded image row. */
    VkDeviceSize source_bytes = 0, destination_bytes = 0;
    void *buffer_address = NULL, *image_address = NULL;
    if (ps5vk_buffer_span(d, buffer, 0, VK_WHOLE_SIZE, &buffer_address, &source_bytes) != VK_SUCCESS ||
        ps5vk_image_span(d, op->copy_image, &image_address, &destination_bytes) != VK_SUCCESS ||
        spans_overlap(buffer_address, source_bytes, image_address, destination_bytes))
        return INVALID;
    struct linear_copy plan;
    if (linear_region_plan(op->copy_image->info.extent.width,
            op->copy_image->info.extent.height, source_bytes, destination_bytes,
            &op->copy_region, &plan)) return INVALID;
    return VK_SUCCESS;
}

VkResult ps5vk_image_linear_region_validate(VkImage image, const VkBufferImageCopy *region,
    VkDeviceSize buffer_bytes, VkDeviceSize image_bytes)
{
    struct linear_copy plan;
    if (!ps5vk_pure_transfer_image(image) || !region) return INVALID;
    return linear_region_plan(image->info.extent.width, image->info.extent.height,
        buffer_bytes, image_bytes, region, &plan) ? INVALID : VK_SUCCESS;
}

VkResult ps5vk_image_linear_execute(VkDevice d, const struct ps5vk_operation *op)
{
    if (!d || !op) return VK_ERROR_DEVICE_LOST;
    if (ps5vk_image_linear_validate(d, op) != VK_SUCCESS) return VK_ERROR_DEVICE_LOST;
    if (op->type == PS5VK_IMAGE_BARRIER) {
        const VkImageMemoryBarrier *b = &op->image_barrier;
        /* The role is host-visible memory that no GPU stage touches, so the
         * transition is bookkeeping and commits in recorded order. UNDEFINED
         * discards the previous contents exactly as the GPU path does. */
        if (b->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED && b->image->layout != b->oldLayout)
            return VK_ERROR_DEVICE_LOST;
        b->image->layout = b->newLayout;
        return VK_SUCCESS;
    }
    void *image_address = NULL, *buffer_address = NULL;
    VkDeviceSize image_bytes = 0, buffer_bytes = 0;
    if (op->copy_image->layout != op->copy_layout ||
        ps5vk_image_span(d, op->copy_image, &image_address, &image_bytes) != VK_SUCCESS ||
        ps5vk_buffer_span(d, op->type == PS5VK_COPY_BUFFER_IMAGE ? op->copy_source : op->copy_destination,
            0, VK_WHOLE_SIZE, &buffer_address, &buffer_bytes) != VK_SUCCESS)
        return VK_ERROR_DEVICE_LOST;
    struct linear_copy plan;
    /* The planner's "buffer" side and "image" side are named, not ordered:
     * only the memcpy argument order depends on the direction. */
    if (linear_region_plan(op->copy_image->info.extent.width,
            op->copy_image->info.extent.height, buffer_bytes, image_bytes,
            &op->copy_region, &plan)) return VK_ERROR_DEVICE_LOST;
    if (!plan.rows || !plan.row_bytes) return VK_ERROR_DEVICE_LOST;
    const VkDeviceSize buffer_span =
        (VkDeviceSize)(plan.rows - 1) * plan.buffer_pitch + plan.row_bytes;
    const VkDeviceSize image_span =
        (VkDeviceSize)(plan.rows - 1) * plan.image_pitch + plan.row_bytes;
    if (plan.buffer_offset > buffer_bytes || buffer_span > buffer_bytes - plan.buffer_offset ||
        plan.image_offset > image_bytes || image_span > image_bytes - plan.image_offset)
        return VK_ERROR_DEVICE_LOST;
    unsigned char *image = (unsigned char *)image_address + plan.image_offset;
    unsigned char *buffer = (unsigned char *)buffer_address + plan.buffer_offset;
    VkResult result = ps5vk_buffer_cache(d,
        op->type == PS5VK_COPY_BUFFER_IMAGE ? op->copy_source : op->copy_destination,
        plan.buffer_offset, buffer_span, VK_TRUE);
    if (result != VK_SUCCESS) return VK_ERROR_DEVICE_LOST;
    for (uint32_t y = 0; y < plan.rows; ++y) {
        unsigned char *image_row = image + (VkDeviceSize)y * plan.image_pitch;
        unsigned char *buffer_row = buffer + (VkDeviceSize)y * plan.buffer_pitch;
        if (op->type == PS5VK_COPY_BUFFER_IMAGE)
            memcpy(image_row, buffer_row, plan.row_bytes);
        else
            memcpy(buffer_row, image_row, plan.row_bytes);
    }
    if (op->type == PS5VK_COPY_BUFFER_IMAGE)
        result = ps5vk_image_flush_range(d, op->copy_image, plan.image_offset, image_span);
    else
        result = ps5vk_buffer_cache(d, op->copy_destination, plan.buffer_offset,
            buffer_span, VK_FALSE);
    return result == VK_SUCCESS ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

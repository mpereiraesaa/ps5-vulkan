#include "vk_command.h"
#include "vk_query_pool.h"
#include "vk_render_pass.h"
#include <string.h>

/* VK_EXT_transform_feedback recording commands (DXVK262-T14).
 *
 * Binding, begin and end are validated against the rules of the pinned
 * registry and recorded as immutable state: vkCmdBindTransformFeedbackBuffersEXT
 * updates the command buffer's bound ranges, and the BEGIN operation snapshots
 * them together with the counter buffers, so the draws recorded until the END
 * operation capture into exactly those ranges. Capture cannot be rebound while
 * it is active, and it ends in the subpass where it began (vkCmdNextSubpass,
 * vkCmdEndRenderPass, vkCmdBindPipeline and vkEndCommandBuffer refuse while it
 * is active, vk_command.c).
 *
 * The device reports transformFeedbackQueries and transformFeedbackDraw false,
 * so a stream query and vkCmdDrawIndirectByteCountEXT are refused; the indexed
 * query commands forward index 0 of every other query type to the core ones.
 * Every refusal invalidates the command buffer and records nothing. */

static int transform_feedback_enabled(VkCommandBuffer c)
{
    return c && c->state == PS5VK_RECORDING &&
        (c->pool->device->enabled_features_t09 & PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK);
}

/* A counter slot: no buffer is legal (capture starts at the bound offset and
 * the count is not stored); a buffer must carry the counter usage and hold the
 * dword at a dword-aligned offset. */
static int counter_range(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset,
                         struct ps5vk_xfb_range *out)
{
    void *address;
    VkDeviceSize bytes;
    memset(out, 0, sizeof(*out));
    if (!buffer) return 1;
    VkDevice d = c->pool->device;
    if (!ps5vk_buffer_usage(d, buffer, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT) ||
        (offset & 3u) ||
        ps5vk_buffer_span(d, buffer, offset, 4u, &address, &bytes) != VK_SUCCESS)
        return 0;
    out->buffer = buffer; out->offset = offset; out->size = 4u;
    return 1;
}

static int counters(VkCommandBuffer c, uint32_t first, uint32_t count,
                    const VkBuffer *buffers, const VkDeviceSize *offsets,
                    struct ps5vk_xfb_range out[PS5VK_XFB_ABI_BUFFERS])
{
    memset(out, 0, sizeof(struct ps5vk_xfb_range) * PS5VK_XFB_ABI_BUFFERS);
    /* VUID-vkCmdBeginTransformFeedbackEXT-firstCounterBuffer-02368/02369. */
    if (first >= PS5VK_XFB_ABI_BUFFERS || count > PS5VK_XFB_ABI_BUFFERS - first) return 0;
    /* A null pCounterBuffers means no counters at all; the offsets array is
     * then ignored, and a null offsets array means zero offsets. */
    if (!count || !buffers) return 1;
    for (uint32_t j = 0; j < count; ++j)
        if (!counter_range(c, buffers[j], offsets ? offsets[j] : 0u, &out[first + j]))
            return 0;
    return 1;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindTransformFeedbackBuffersEXT(VkCommandBuffer c,
    uint32_t first, uint32_t count, const VkBuffer *buffers,
    const VkDeviceSize *offsets, const VkDeviceSize *sizes)
{
    if (!c) return;
    if (!transform_feedback_enabled(c) || c->xfb_active || !count || !buffers || !offsets ||
        first >= PS5VK_XFB_ABI_BUFFERS || count > PS5VK_XFB_ABI_BUFFERS - first)
        { ps5vk_command_invalidate(c); return; }
    struct ps5vk_xfb_range ranges[PS5VK_XFB_ABI_BUFFERS];
    VkDevice d = c->pool->device;
    for (uint32_t j = 0; j < count; ++j) {
        void *address;
        VkDeviceSize bytes;
        const VkDeviceSize size = sizes ? sizes[j] : VK_WHOLE_SIZE;
        /* VUID-vkCmdBindTransformFeedbackBuffersEXT-pOffsets-02358..02362:
         * transform feedback usage, a dword-aligned offset inside the buffer,
         * a non-zero size inside it, and no more than
         * maxTransformFeedbackBufferSize. The buffer must be bound to memory. */
        if (!buffers[j] ||
            !ps5vk_buffer_usage(d, buffers[j], VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT) ||
            (offsets[j] & 3u) || !size ||
            ps5vk_buffer_span(d, buffers[j], offsets[j], size, &address, &bytes) != VK_SUCCESS ||
            !bytes || bytes > PS5VK_XFB_MAX_BUFFER_SIZE)
            { ps5vk_command_invalidate(c); return; }
        ranges[j] = (struct ps5vk_xfb_range){buffers[j], offsets[j], bytes};
    }
    memcpy(&c->xfb_bindings[first], ranges, count * sizeof(ranges[0]));
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginTransformFeedbackEXT(VkCommandBuffer c,
    uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets)
{
    if (!c) return;
    struct ps5vk_xfb_operation record;
    memset(&record, 0, sizeof(record));
    const VkPipeline p = c->graphics_pipeline;
    uint32_t bound = 0;
    for (uint32_t j = 0; j < PS5VK_XFB_ABI_BUFFERS; ++j)
        if (c->xfb_bindings[j].buffer) bound |= 1u << j;
    /* Inside a render pass instance of a primary, not already active, with a
     * bound pipeline whose last pre-rasterization stage declares Xfb (04128)
     * and whose captured buffers are all bound; a multiview subpass cannot
     * capture (02373). */
    if (!transform_feedback_enabled(c) || c->xfb_active ||
        c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || !c->render_pass ||
        c->render_pass_inherited ||
        (c->render_pass->multiview.present &&
         c->render_pass->multiview.view_masks[c->subpass]) ||
        !p || !p->xfb.buffers_mask || (p->xfb.buffers_mask & ~bound) ||
        !counters(c, first, count, buffers, offsets, record.counters))
        { ps5vk_command_invalidate(c); return; }
    memcpy(record.buffers, c->xfb_bindings, sizeof(record.buffers));
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(c,
        PS5VK_TRANSFORM_FEEDBACK_BEGIN, PS5VK_OPERATION_INSIDE_RENDER_PASS, 1);
    if (!op) return;
    op->xfb = record;
    op->render_pass = c->render_pass; op->framebuffer = c->framebuffer;
    op->subpass = c->subpass;
    c->xfb_active = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndTransformFeedbackEXT(VkCommandBuffer c,
    uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets)
{
    if (!c) return;
    struct ps5vk_xfb_operation record;
    memset(&record, 0, sizeof(record));
    if (!transform_feedback_enabled(c) || !c->xfb_active ||
        !counters(c, first, count, buffers, offsets, record.counters))
        { ps5vk_command_invalidate(c); return; }
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(c,
        PS5VK_TRANSFORM_FEEDBACK_END, PS5VK_OPERATION_INSIDE_RENDER_PASS, 1);
    if (!op) return;
    op->xfb = record;
    op->render_pass = c->render_pass; op->framebuffer = c->framebuffer;
    op->subpass = c->subpass;
    c->xfb_active = VK_FALSE;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginQueryIndexedEXT(VkCommandBuffer c, VkQueryPool pool,
    uint32_t query, VkQueryControlFlags flags, uint32_t index)
{
    if (!c) return;
    if (!transform_feedback_enabled(c) || !pool || index ||
        pool->query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT)
        { ps5vk_command_invalidate(c); return; }
    vkCmdBeginQuery(c, pool, query, flags);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndQueryIndexedEXT(VkCommandBuffer c, VkQueryPool pool,
    uint32_t query, uint32_t index)
{
    if (!c) return;
    if (!transform_feedback_enabled(c) || !pool || index ||
        pool->query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT)
        { ps5vk_command_invalidate(c); return; }
    vkCmdEndQuery(c, pool, query);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectByteCountEXT(VkCommandBuffer c,
    uint32_t instances, uint32_t first_instance, VkBuffer counter,
    VkDeviceSize counter_offset, uint32_t counter_bias, uint32_t stride)
{
    (void)instances; (void)first_instance; (void)counter; (void)counter_offset;
    (void)counter_bias; (void)stride;
    /* transformFeedbackDraw is reported false (VUID-02288). */
    if (c) ps5vk_command_invalidate(c);
}

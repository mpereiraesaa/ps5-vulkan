/* VK_KHR_synchronization2 on the Vulkan 1.0 profile: the stage2/access2/
 * layout conversion, and the barrier shapes pinned DXVK 2.6.2 records for its
 * first frame (clear, render, readback, hand-back), each checked against what
 * the Vulkan 1.0 recorder then accepts or refuses. */
#include "vk_sync2.h"
#include "vk_image.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static VkResult allocate(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx;
    *address = *backing = calloc(1, (size_t)size);
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void release(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult sync_memory(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult image_requirements(VkDevice d, const VkImageCreateInfo *info,
                                   VkMemoryRequirements *out)
{
    (void)d;
    if ((info->format != VK_FORMAT_B8G8R8A8_UNORM &&
         info->format != VK_FORMAT_R8G8B8A8_UNORM) ||
        info->extent.width > 64 || info->extent.height > 64)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out = (VkMemoryRequirements){65536, 256, 1};
    return VK_SUCCESS;
}
static VkDeviceMemory memory(VkDevice d, VkDeviceSize size)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = size, .memoryTypeIndex = 0};
    VkDeviceMemory out = VK_NULL_HANDLE;
    assert(vkAllocateMemory(d, &info, NULL, &out) == VK_SUCCESS);
    return out;
}
static void recording(struct VkCommandBuffer_T *command, VkCommandPool pool)
{ *command = (struct VkCommandBuffer_T){.pool = pool, .state = PS5VK_RECORDING}; }

#define S2(x) VK_PIPELINE_STAGE_2_##x##_BIT
#define A2(x) VK_ACCESS_2_##x##_BIT
#define S1(x) VK_PIPELINE_STAGE_##x##_BIT
#define A1(x) VK_ACCESS_##x##_BIT

static void conversion(void)
{
    VkPipelineStageFlags s;
    VkAccessFlags a;
    /* Core bits keep their value. */
    assert(ps5vk_sync2_stage_mask(S2(COLOR_ATTACHMENT_OUTPUT) | S2(TRANSFER), VK_TRUE, &s) &&
           s == (S1(COLOR_ATTACHMENT_OUTPUT) | S1(TRANSFER)));
    /* The split transfer stages are all TRANSFER. */
    const VkPipelineStageFlags2 split[] = {S2(COPY), S2(RESOLVE), S2(BLIT), S2(CLEAR)};
    for (unsigned i = 0; i < 4; ++i)
        assert(ps5vk_sync2_stage_mask(split[i], VK_FALSE, &s) && s == S1(TRANSFER));
    assert(ps5vk_sync2_stage_mask(S2(INDEX_INPUT) | S2(VERTEX_ATTRIBUTE_INPUT), VK_TRUE, &s) &&
           s == S1(VERTEX_INPUT));
    assert(ps5vk_sync2_stage_mask(S2(PRE_RASTERIZATION_SHADERS), VK_TRUE, &s) &&
           s == (S1(VERTEX_SHADER) | S1(TESSELLATION_CONTROL_SHADER) |
                 S1(TESSELLATION_EVALUATION_SHADER) | S1(GEOMETRY_SHADER)));
    /* NONE: TOP_OF_PIPE as a source, BOTTOM_OF_PIPE as a destination. */
    assert(ps5vk_sync2_stage_mask(VK_PIPELINE_STAGE_2_NONE, VK_TRUE, &s) &&
           s == S1(TOP_OF_PIPE));
    assert(ps5vk_sync2_stage_mask(VK_PIPELINE_STAGE_2_NONE, VK_FALSE, &s) &&
           s == S1(BOTTOM_OF_PIPE));
    /* No Vulkan 1.0 equivalent: refused. */
    assert(!ps5vk_sync2_stage_mask(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                   VK_TRUE, &s));
    assert(!ps5vk_sync2_stage_mask(VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
                                   VK_TRUE, &s));
    assert(!ps5vk_sync2_stage_mask(VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_TRUE, &s));

    assert(ps5vk_sync2_access_mask(A2(COLOR_ATTACHMENT_WRITE) | A2(TRANSFER_READ), &a) &&
           a == (A1(COLOR_ATTACHMENT_WRITE) | A1(TRANSFER_READ)));
    assert(ps5vk_sync2_access_mask(A2(SHADER_SAMPLED_READ) | A2(SHADER_STORAGE_READ), &a) &&
           a == A1(SHADER_READ));
    assert(ps5vk_sync2_access_mask(A2(SHADER_STORAGE_WRITE), &a) && a == A1(SHADER_WRITE));
    assert(ps5vk_sync2_access_mask(VK_ACCESS_2_NONE, &a) && a == 0);
    assert(!ps5vk_sync2_access_mask(VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT, &a));
    assert(!ps5vk_sync2_access_mask(VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT, &a));

    VkImageLayout l;
    assert(ps5vk_sync2_image_layout(VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, NULL,
               VK_IMAGE_ASPECT_COLOR_BIT, &l) && l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    assert(ps5vk_sync2_image_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, NULL,
               VK_IMAGE_ASPECT_COLOR_BIT, &l) && l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(ps5vk_sync2_image_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, NULL,
               VK_IMAGE_ASPECT_COLOR_BIT, &l) && l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    struct VkImage_T depth = {.info = {.format = VK_FORMAT_D32_SFLOAT}};
    struct VkImage_T combined = {.info = {.format = VK_FORMAT_D24_UNORM_S8_UINT}};
    assert(ps5vk_sync2_image_layout(VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, &depth,
               VK_IMAGE_ASPECT_DEPTH_BIT, &l) &&
           l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    assert(ps5vk_sync2_image_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, &combined,
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &l) &&
           l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    /* One aspect of a combined format, or a mixed aspect: ambiguous. */
    assert(!ps5vk_sync2_image_layout(VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, &combined,
               VK_IMAGE_ASPECT_DEPTH_BIT, &l));
    assert(!ps5vk_sync2_image_layout(VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, &depth,
               VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT, &l));

    /* Whole dependency: an access without a stage, a pNext and an unknown bit
     * refuse the whole structure. */
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = S2(COPY), .srcAccessMask = A2(TRANSFER_WRITE),
        .dstStageMask = S2(HOST), .dstAccessMask = A2(HOST_READ)};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &m};
    struct ps5vk_sync2_barrier *list; uint32_t n;
    assert(ps5vk_sync2_convert_dependency(&dep, &list, &n) == VK_SUCCESS && n == 1);
    assert(list[0].src_stage == S1(TRANSFER) && list[0].dst_stage == S1(HOST) &&
           list[0].memory.srcAccessMask == A1(TRANSFER_WRITE) &&
           list[0].memory.dstAccessMask == A1(HOST_READ));
    free(list);
    m.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    assert(ps5vk_sync2_convert_dependency(&dep, &list, &n) == VK_ERROR_UNKNOWN && !list);
    m.srcStageMask = S2(COPY);
    m.pNext = &dep;
    assert(ps5vk_sync2_convert_dependency(&dep, &list, &n) == VK_ERROR_UNKNOWN);
    m.pNext = NULL;
    m.dstAccessMask = VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR;
    assert(ps5vk_sync2_convert_dependency(&dep, &list, &n) == VK_ERROR_UNKNOWN);
    m.dstAccessMask = A2(HOST_READ);
    dep.pNext = &m;
    assert(ps5vk_sync2_convert_dependency(&dep, &list, &n) == VK_ERROR_UNKNOWN);
}

struct fixture {
    struct VkDevice_T d;
    VkImage image;
    VkDeviceMemory image_memory;
    struct VkCommandPool_T pool;
};

static VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = ss, .srcAccessMask = sa, .dstStageMask = ds, .dstAccessMask = da,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}
static VkDependencyInfo one_image(const VkImageMemoryBarrier2 *b)
{
    return (VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = b};
}

/* The masks DXVK 2.6.2 names (dxvk_barrier.cpp, first-frame trace). */
#define DXVK_RT_STAGES (S2(COLOR_ATTACHMENT_OUTPUT) | S2(TRANSFER))              /* 0x1400 */
#define DXVK_RT_ACCESS (A2(COLOR_ATTACHMENT_READ) | A2(COLOR_ATTACHMENT_WRITE) | \
                        A2(TRANSFER_READ) | A2(TRANSFER_WRITE))                   /* 0x1980 */
#define DXVK_HOST_STAGES (S2(HOST) | S2(TRANSFER) | S2(COMPUTE_SHADER) | \
                          S2(FRAGMENT_SHADER))                                    /* 0x5880 */
#define DXVK_HOST_ACCESS (A2(HOST_READ) | A2(TRANSFER_WRITE) | A2(TRANSFER_READ) | \
                          A2(SHADER_WRITE) | A2(SHADER_READ))                     /* 0x3860 */

static void dxvk_first_frame(struct fixture *f)
{
    struct VkCommandBuffer_T c;
    const VkImageLayout U = VK_IMAGE_LAYOUT_UNDEFINED,
        DST = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        SRC = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        ATT = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    /* InitBarriers: UNDEFINED -> TRANSFER_DST, src NONE/NONE. Accepted as the
     * attachment-initialisation acquire, TOP_OF_PIPE -> TRANSFER. */
    recording(&c, &f->pool);
    VkImageMemoryBarrier2 b = image_barrier(f->image, U, DST, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, S2(TRANSFER), A2(TRANSFER_WRITE));
    VkDependencyInfo dep = one_image(&b);
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 1);
    assert(c.operations[0].type == PS5VK_IMAGE_BARRIER &&
           c.operations[0].src_stage == S1(TOP_OF_PIPE) &&
           c.operations[0].dst_stage == S1(TRANSFER) &&
           c.operations[0].image_barrier.oldLayout == U &&
           c.operations[0].image_barrier.newLayout == DST &&
           c.operations[0].src_access == 0 && c.operations[0].dst_access == A1(TRANSFER_WRITE));

    /* InitBuffer hand-over: TRANSFER_DST -> COLOR_ATTACHMENT, src
     * TRANSFER/TRANSFER_WRITE, dst 0x1400/0x1980. DXVK's destination names
     * TRANSFER_WRITE, which the Vulkan 1.0 hand-over profile does not order
     * for an attachment, so the converted barrier is refused... */
    b = image_barrier(f->image, DST, ATT, S2(TRANSFER), A2(TRANSFER_WRITE),
                      DXVK_RT_STAGES, DXVK_RT_ACCESS);
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_INVALID && c.operation_count == 1);
    /* ...and without it the same conversion records the hand-over. */
    recording(&c, &f->pool);
    b.dstAccessMask = DXVK_RT_ACCESS & ~A2(TRANSFER_WRITE);
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 1);
    assert(c.operations[0].src_stage == S1(TRANSFER) &&
           c.operations[0].dst_stage == (S1(COLOR_ATTACHMENT_OUTPUT) | S1(TRANSFER)) &&
           c.operations[0].src_access == A1(TRANSFER_WRITE) &&
           c.operations[0].dst_access == (A1(COLOR_ATTACHMENT_READ) |
               A1(COLOR_ATTACHMENT_WRITE) | A1(TRANSFER_READ)));

    /* Exec: discard into the attachment layout, src COLOR_OUTPUT/NONE. */
    recording(&c, &f->pool);
    b = image_barrier(f->image, U, ATT, S2(COLOR_ATTACHMENT_OUTPUT), VK_ACCESS_2_NONE,
                      S2(COLOR_ATTACHMENT_OUTPUT), A2(COLOR_ATTACHMENT_WRITE));
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 1 &&
           c.operations[0].image_barrier.newLayout == ATT &&
           c.operations[0].src_stage == S1(COLOR_ATTACHMENT_OUTPUT));

    /* After the pass: the global render-target publication, one aggregate. */
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = S2(COLOR_ATTACHMENT_OUTPUT), .srcAccessMask = A2(COLOR_ATTACHMENT_WRITE),
        .dstStageMask = DXVK_RT_STAGES, .dstAccessMask = DXVK_RT_ACCESS};
    VkDependencyInfo mdep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &m};
    vkCmdPipelineBarrier2KHR(&c, &mdep);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 2 &&
           c.operations[1].type == PS5VK_BARRIER &&
           c.operations[1].src_access == A1(COLOR_ATTACHMENT_WRITE));

    /* The readback transition with the access the Vulkan 1.0 profile knows
     * (COLOR_WRITE -> TRANSFER_READ) is accepted... */
    b = image_barrier(f->image, ATT, SRC, S2(COLOR_ATTACHMENT_OUTPUT),
                      A2(COLOR_ATTACHMENT_WRITE), S2(COPY), A2(TRANSFER_READ));
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 3 &&
           c.operations[2].dst_stage == S1(TRANSFER) &&
           c.operations[2].image_barrier.newLayout == SRC);

    /* ...while DXVK's own form names src TRANSFER/NONE, relying on the
     * global publication just before it. The conversion is exact, and the
     * Vulkan 1.0 profile refuses a layout transition without the producer's
     * write, so the command buffer is invalidated rather than guessed. */
    recording(&c, &f->pool);
    b = image_barrier(f->image, ATT, SRC, S2(TRANSFER), VK_ACCESS_2_NONE,
                      S2(TRANSFER), A2(TRANSFER_READ));
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_INVALID && !c.operation_count);

    /* The final dependency, exactly as DXVK records it: a global
     * TRANSFER_WRITE publication to 0x5880/0x3860 plus the image hand-back
     * TRANSFER_SRC -> COLOR_ATTACHMENT at 0x1400/0x1980. It becomes one
     * Vulkan 1.0 barrier whose stages are the union of both members (dst
     * 0x5c80): the publication's stages alone name no colour-attachment
     * stage for the hand-back's colour access. */
    VkMemoryBarrier2 publish = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = S2(TRANSFER), .srcAccessMask = A2(TRANSFER_WRITE),
        .dstStageMask = DXVK_HOST_STAGES, .dstAccessMask = DXVK_HOST_ACCESS};
    VkImageMemoryBarrier2 back = image_barrier(f->image, SRC, ATT, S2(TRANSFER),
        VK_ACCESS_2_NONE, DXVK_RT_STAGES, DXVK_RT_ACCESS);
    VkDependencyInfo last = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &publish,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back};
    {
        struct ps5vk_sync2_barrier *list; uint32_t n;
        VkPipelineStageFlags src, dst;
        assert(ps5vk_sync2_convert_dependency(&last, &list, &n) == VK_SUCCESS && n == 2);
        ps5vk_sync2_union_stages(list, n, &src, &dst);
        assert(src == S1(TRANSFER) && dst == 0x5c80u &&
               dst == (S1(HOST) | S1(TRANSFER) | S1(COMPUTE_SHADER) |
                       S1(FRAGMENT_SHADER) | S1(COLOR_ATTACHMENT_OUTPUT)));
        assert(list[0].memory.dstAccessMask == 0x3860u &&
               list[1].image.dstAccessMask == 0x1980u && !list[1].image.srcAccessMask);
        free(list);
    }
    /* With DXVK's src NONE the Vulkan 1.0 image profile still refuses the
     * hand-back; with the readback's read it records publication then image,
     * both under the union stages. */
    recording(&c, &f->pool);
    vkCmdPipelineBarrier2KHR(&c, &last);
    assert(c.state == PS5VK_INVALID && !c.operation_count);
    recording(&c, &f->pool);
    back.srcAccessMask = A2(TRANSFER_READ);
    back.dstStageMask = S2(COLOR_ATTACHMENT_OUTPUT);
    back.dstAccessMask = A2(COLOR_ATTACHMENT_WRITE);
    vkCmdPipelineBarrier2KHR(&c, &last);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 2);
    assert(c.operations[0].type == PS5VK_BARRIER &&
           c.operations[0].src_stage == S1(TRANSFER) &&
           c.operations[0].dst_stage == 0x5c80u &&
           c.operations[0].dst_access == (A1(HOST_READ) | A1(TRANSFER_WRITE) |
               A1(TRANSFER_READ) | A1(SHADER_WRITE) | A1(SHADER_READ)));
    assert(c.operations[1].type == PS5VK_IMAGE_BARRIER &&
           c.operations[1].dst_stage == 0x5c80u &&
           c.operations[1].image_barrier.oldLayout == SRC &&
           c.operations[1].image_barrier.newLayout == ATT);

    /* Members that share one stage pair keep that pair. */
    recording(&c, &f->pool);
    publish.dstStageMask = S2(COLOR_ATTACHMENT_OUTPUT);
    publish.dstAccessMask = A2(COLOR_ATTACHMENT_WRITE);
    vkCmdPipelineBarrier2KHR(&c, &last);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 2 &&
           c.operations[0].type == PS5VK_BARRIER && c.operations[1].type == PS5VK_IMAGE_BARRIER &&
           c.operations[0].dst_stage == S1(COLOR_ATTACHMENT_OUTPUT) &&
           c.operations[1].dst_stage == S1(COLOR_ATTACHMENT_OUTPUT));

    /* ATTACHMENT_OPTIMAL is the colour attachment layout of a colour range. */
    recording(&c, &f->pool);
    b = image_barrier(f->image, U, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                      S2(COLOR_ATTACHMENT_OUTPUT), VK_ACCESS_2_NONE,
                      S2(COLOR_ATTACHMENT_OUTPUT), A2(COLOR_ATTACHMENT_WRITE));
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 1 &&
           c.operations[0].image_barrier.newLayout == ATT);

    /* Refusals leave nothing behind: an unrepresentable stage, dependency
     * flags the 1.0 recorder refuses, and a barrier pNext. */
    recording(&c, &f->pool);
    b = image_barrier(f->image, U, ATT, VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
                      VK_ACCESS_2_NONE, S2(COLOR_ATTACHMENT_OUTPUT), A2(COLOR_ATTACHMENT_WRITE));
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_INVALID && !c.operation_count);
    recording(&c, &f->pool);
    b.srcStageMask = S2(COLOR_ATTACHMENT_OUTPUT);
    dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_INVALID && !c.operation_count);
    dep.dependencyFlags = 0;
    recording(&c, &f->pool);
    b.pNext = &m;
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_INVALID && !c.operation_count);
    b.pNext = NULL;

    /* Swapchain present release as the pinned WSI blitter records it. */
    recording(&c, &f->pool);
    f->image->swapchain_owned = VK_TRUE;
    b = image_barrier(f->image, ATT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      S2(COLOR_ATTACHMENT_OUTPUT), A2(COLOR_ATTACHMENT_WRITE),
                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 1 &&
           c.operations[0].dst_stage == S1(BOTTOM_OF_PIPE) &&
           c.operations[0].image_barrier.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    f->image->swapchain_owned = VK_FALSE;
}

static void events_and_timestamps(struct fixture *f)
{
    struct VkCommandBuffer_T c;
    VkEventCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent event;
    assert(vkCreateEvent(&f->d, &ei, NULL, &event) == VK_SUCCESS);
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = S2(COPY), .srcAccessMask = A2(TRANSFER_WRITE),
        .dstStageMask = S2(HOST), .dstAccessMask = A2(HOST_READ)};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &m};

    recording(&c, &f->pool);
    vkCmdSetEvent2KHR(&c, event, &dep);
    assert(c.state == PS5VK_RECORDING && c.operation_count == 1 &&
           c.operations[0].type == PS5VK_EVENT_SET && c.operations[0].src_stage == S1(TRANSFER));
    vkCmdWaitEvents2KHR(&c, 1, &event, &dep);
    assert(c.state == PS5VK_RECORDING && c.operation_count > 1 &&
           c.operations[1].type == PS5VK_EVENT_WAIT);
    vkCmdResetEvent2KHR(&c, event, S2(BLIT));
    assert(c.state == PS5VK_RECORDING &&
           c.operations[c.operation_count - 1].type == PS5VK_EVENT_RESET &&
           c.operations[c.operation_count - 1].src_stage == S1(TRANSFER));

    /* Set takes no dependency flags; reset refuses an unrepresentable stage. */
    recording(&c, &f->pool);
    dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    vkCmdSetEvent2KHR(&c, event, &dep);
    assert(c.state == PS5VK_INVALID && !c.operation_count);
    dep.dependencyFlags = 0;
    recording(&c, &f->pool);
    vkCmdResetEvent2KHR(&c, event, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    assert(c.state == PS5VK_INVALID && !c.operation_count);
    recording(&c, &f->pool);
    vkCmdWaitEvents2KHR(&c, 0, &event, &dep);
    assert(c.state == PS5VK_INVALID);

    /* The queue family reports timestampValidBits = 0: the converted
     * timestamp reaches the Vulkan 1.0 command, which refuses it, and a
     * multi-bit or unrepresentable stage is refused before that. */
    recording(&c, &f->pool);
    vkCmdWriteTimestamp2KHR(&c, S2(COPY) | S2(BLIT), VK_NULL_HANDLE, 0);
    assert(c.state == PS5VK_INVALID);
    recording(&c, &f->pool);
    vkCmdWriteTimestamp2KHR(&c, S2(BOTTOM_OF_PIPE), VK_NULL_HANDLE, 0);
    assert(c.state == PS5VK_INVALID && !c.operation_count);
    vkDestroyEvent(&f->d, event, NULL);
}

static void feature_gate(struct fixture *f)
{
    /* Without the synchronization2 feature every command fails closed. */
    struct VkCommandBuffer_T c;
    f->d.enabled_features_t09 = 0;
    VkImageMemoryBarrier2 b = image_barrier(f->image, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, S2(COLOR_ATTACHMENT_OUTPUT),
        VK_ACCESS_2_NONE, S2(COLOR_ATTACHMENT_OUTPUT), A2(COLOR_ATTACHMENT_WRITE));
    VkDependencyInfo dep = one_image(&b);
    recording(&c, &f->pool);
    vkCmdPipelineBarrier2KHR(&c, &dep);
    assert(c.state == PS5VK_INVALID && !c.operation_count);
    recording(&c, &f->pool);
    vkCmdResetEvent2KHR(&c, VK_NULL_HANDLE, S2(TRANSFER));
    assert(c.state == PS5VK_INVALID);
    f->d.queue.device = &f->d;
    assert(vkQueueSubmit2KHR(&f->d.queue, 0, NULL, VK_NULL_HANDLE) == VK_ERROR_UNKNOWN);
    f->d.enabled_features_t09 = PS5VK_T09_FEATURE_SYNCHRONIZATION2;
}

int main(void)
{
    conversion();

    static struct fixture f;
    f.d = (struct VkDevice_T){.graphics_enabled = VK_TRUE,
        .enabled_features_t09 = PS5VK_T09_FEATURE_SYNCHRONIZATION2,
        .max_allocation = 1u << 20, .buffer_alignment = 16,
        .noncoherent_atom = 1, .image_requirements = image_requirements,
        .memory = {.allocate = allocate, .release = release,
                   .flush = sync_memory, .invalidate = sync_memory}};
    /* The render target of DXVK's first frame: RGBA8 64x64, colour
     * attachment and transfer source and destination (usage 0x13). */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    assert(vkCreateImage(&f.d, &ii, NULL, &f.image) == VK_SUCCESS);
    f.image_memory = memory(&f.d, 65536);
    assert(vkBindImageMemory(&f.d, f.image, f.image_memory, 0) == VK_SUCCESS);
    f.pool = (struct VkCommandPool_T){.device = &f.d};

    dxvk_first_frame(&f);
    events_and_timestamps(&f);
    feature_gate(&f);

    vkDestroyImage(&f.d, f.image, NULL);
    vkFreeMemory(&f.d, f.image_memory, NULL);
    assert(!f.d.buffers && !f.d.images && !f.d.memories);
    puts("VK_KHR_synchronization2 conversion and DXVK first-frame barriers: pass "
         "(host recording only)");
    return 0;
}

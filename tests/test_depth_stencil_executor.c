/*
 * T09 native executor contracts for the combined D32_SFLOAT_S8_UINT
 * attachment: per-aspect barriers, the per-aspect readback shapes, the
 * per-aspect render-pass plan and the plane-by-plane detile. Host only: the
 * GPU side is the SDK witness (examples/t09_depth_stencil_witness).
 */
#include "upload_commands_ps5.h"
#include "readback_commands_ps5.h"
#include "attachment_ops.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { EXTENT = 64, PLANES = 131072 };
static _Alignas(65536) unsigned char surface[PLANES];
static unsigned char host[EXTENT * EXTENT * 4];
static uint32_t words[4096];
VkResult ps5vk_image_span(VkDevice d, VkImage i, void **p, VkDeviceSize *n)
{ assert(d && i); *p = surface; *n = sizeof(surface); return VK_SUCCESS; }
VkResult ps5vk_buffer_span(VkDevice d, VkBuffer b, VkDeviceSize offset, VkDeviceSize size,
    void **p, VkDeviceSize *n)
{ assert(d && b); (void)offset; (void)size; *p = host; *n = sizeof(host); return VK_SUCCESS; }
VkResult ps5vk_texture_copy_plan_for_image(VkImage image, VkDeviceSize src, VkDeviceSize dst,
    const VkBufferImageCopy *region, struct ps5vk_texture_copy *out)
{ (void)image; (void)src; (void)dst; (void)region; (void)out; return VK_ERROR_UNKNOWN; }
static void flush(const void *p, size_t n) { (void)p; (void)n; }

#define DA VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
#define SA VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL
#define DSA VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
#define SRC VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
#define D VK_IMAGE_ASPECT_DEPTH_BIT
#define S VK_IMAGE_ASPECT_STENCIL_BIT

static struct ps5vk_operation barrier(VkImage image, VkImageAspectFlags aspects,
    VkImageLayout old, VkImageLayout next)
{
    return (struct ps5vk_operation){.type = PS5VK_IMAGE_BARRIER,
        .src_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .image_barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = old, .newLayout = next, .image = image,
            .subresourceRange = {aspects, 0, 1, 0, 1}}};
}

static void readback_ops(struct ps5vk_operation *ops, VkImage image, VkImageAspectFlags aspect,
    VkBuffer buffer)
{
    ops[0] = (struct ps5vk_operation){.type = PS5VK_COPY_IMAGE_BUFFER, .copy_image = image,
        .copy_destination = buffer, .copy_layout = SRC,
        .copy_region = {.imageSubresource = {aspect, 0, 0, 1}, .imageExtent = {EXTENT, EXTENT, 1}}};
    ops[1] = (struct ps5vk_operation){.type = PS5VK_BARRIER,
        .src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT, .dst_stage = VK_PIPELINE_STAGE_HOST_BIT,
        .src_access = VK_ACCESS_TRANSFER_WRITE_BIT, .dst_access = VK_ACCESS_HOST_READ_BIT,
        .buffer_barrier = {.buffer = buffer, .size = VK_WHOLE_SIZE}};
    ops[2] = (struct ps5vk_operation){.type = PS5VK_BARRIER,
        .src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT, .dst_stage = VK_PIPELINE_STAGE_HOST_BIT};
}

static VkResult upload(VkDevice d, const struct ps5vk_operation *op,
    struct ps5vk_layout_state *layouts)
{
    uint32_t *cursor = words;
    return ps5vk_upload_commands(d, op, 1, NULL, layouts, &cursor, words + 4096, flush);
}

int main(void)
{
    struct VkDevice_T device = {0};
    struct VkImage_T image = {.device = &device, .info = {
        .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .imageType = VK_IMAGE_TYPE_2D,
        .extent = {EXTENT, EXTENT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT}};
    VkBuffer buffer = (VkBuffer)(uintptr_t)1;
    assert(ps5vk_depth_stencil_attachment_image(&image));

    /* --- recorder-side barrier shape -------------------------------------- */
    struct ps5vk_operation op = barrier(&image, D, DA, SRC);
    assert(ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));
    /* Without separateDepthStencilLayouts: both aspects, combined layouts. */
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_FALSE));
    op = barrier(&image, D | S, DSA, SRC);
    assert(ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_FALSE));
    op = barrier(&image, D | S, DA, SRC);   /* separate layout, no feature */
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_FALSE));
    op = barrier(&image, D | S, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL, SRC);
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_FALSE));
    assert(ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));
    /* VK_KHR_maintenance2 alone admits the mixed layouts on both aspects,
     * never a single aspect or a separate layout. */
    assert(ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_TRUE));
    op = barrier(&image, D | S, DSA, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL);
    assert(ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_TRUE));
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_FALSE));
    op = barrier(&image, D, DSA, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL);
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_TRUE));
    op = barrier(&image, D | S, DA, SRC);
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_FALSE, VK_TRUE));
    /* A layout that names the other aspect, a colour layout, a colour aspect,
     * a transfer destination (no such usage) and a foreign access. */
    op = barrier(&image, S, DA, SRC);
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));
    op = barrier(&image, D, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));
    op = barrier(&image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, SRC);
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));
    op = barrier(&image, S, SA, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));
    op = barrier(&image, S, SA, SRC);
    op.image_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));
    op = barrier(&image, S, SA, VK_IMAGE_LAYOUT_UNDEFINED);   /* UNDEFINED as new */
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));
    /* The readback source needs the image's TRANSFER_SRC usage. */
    struct VkImage_T attachment_only = image;
    attachment_only.info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    op = barrier(&attachment_only, D, DA, SRC);
    assert(!ps5vk_depth_stencil_barrier(&op.image_barrier, VK_TRUE, VK_FALSE));

    /* --- executor: per-aspect transitions ---------------------------------- */
    struct ps5vk_layout_state layouts = {0};
    op = barrier(&image, D | S, VK_IMAGE_LAYOUT_UNDEFINED, DSA);
    op.image_barrier.srcAccessMask = 0;
    assert(upload(&device, &op, &layouts) == VK_SUCCESS);
    op = barrier(&image, D, DA, SRC);
    assert(upload(&device, &op, &layouts) == VK_SUCCESS);
    assert(ps5vk_layout_require_aspects(&layouts, &image, S, SA) == VK_SUCCESS);
    assert(ps5vk_layout_require_aspects(&layouts, &image, D, SRC) == VK_SUCCESS);
    /* Wrong old layout for the stencil aspect: refused, nothing staged. */
    struct ps5vk_layout_state before = layouts;
    op = barrier(&image, S, SRC, SA);
    assert(upload(&device, &op, &layouts) != VK_SUCCESS);
    assert(!memcmp(&before, &layouts, sizeof(layouts)));

    /* --- readback: unstaged copy of an aspect not in TRANSFER_SRC ---------- */
    struct ps5vk_operation ops[4];
    struct ps5vk_readback_plan plan = {0};
    unsigned site = 0;
    readback_ops(ops, &image, S, buffer);
    before = layouts;
    assert(ps5vk_readback_commands(&device, ops, 3, NULL, &layouts, &plan, &site) != VK_SUCCESS);
    assert(!memcmp(&before, &layouts, sizeof(layouts)) && !plan.image);
    /* The depth aspect is in TRANSFER_SRC: its unstaged copy is accepted. */
    readback_ops(ops, &image, D, buffer);
    assert(ps5vk_readback_commands(&device, ops, 3, NULL, &layouts, &plan, &site) == VK_SUCCESS);
    assert(plan.image == &image && plan.aspect == D && plan.buffer == buffer);
    /* Staged stencil: the barrier moves the stencil aspect only. */
    ops[0] = barrier(&image, S, SA, SRC);
    readback_ops(ops + 1, &image, S, buffer);
    assert(ps5vk_readback_commands(&device, ops, 4, NULL, &layouts, &plan, &site) == VK_SUCCESS);
    assert(plan.aspect == S);
    assert(ps5vk_layout_require(&layouts, &image, SRC) == VK_SUCCESS);
    /* A staged barrier that does not name the copied aspect is refused. */
    struct ps5vk_layout_state other = {0};
    assert(ps5vk_layout_transition(&other, &image, VK_IMAGE_LAYOUT_UNDEFINED, DSA) == VK_SUCCESS);
    ops[0] = barrier(&image, D, DA, SRC);
    readback_ops(ops + 1, &image, S, buffer);
    before = other;
    assert(ps5vk_readback_commands(&device, ops, 4, NULL, &other, &plan, &site) != VK_SUCCESS);
    assert(!memcmp(&before, &other, sizeof(other)));
    /* A copy of both aspects at once, or of the colour aspect, is refused. */
    readback_ops(ops, &image, D | S, buffer);
    assert(ps5vk_readback_commands(&device, ops, 3, NULL, &layouts, &plan, &site) != VK_SUCCESS);
    /* A draw target handed in as `color` never owns this shape. */
    readback_ops(ops, &image, D, buffer);
    assert(ps5vk_readback_commands(&device, ops, 3, &image, &layouts, &plan, &site) != VK_SUCCESS);

    /* --- detile: each aspect from its own plane ---------------------------- */
    struct ps5vk_depth_stencil_layout planes;
    assert(!ps5vk_depth_stencil_layout(EXTENT, EXTENT, &planes) && planes.bytes == PLANES);
    for (uint32_t y = 0; y < EXTENT; ++y)
        for (uint32_t x = 0; x < EXTENT; ++x) {
            const float depth = x < 32 ? 0.25f : 1.0f;
            memcpy(surface + ps5vk_depth_64k_zx_offset(x, y, EXTENT), &depth, 4);
            surface[planes.stencil_offset + ps5vk_stencil_64k_zx_offset(x, y, EXTENT)] =
                x < 32 ? 0x5a : 0x17;
        }
    assert(!ps5vk_depth_stencil_readback_detile(&image, D, host, sizeof(host),
                                                surface, sizeof(surface)));
    for (uint32_t i = 0; i < EXTENT * EXTENT; ++i) {
        float value; memcpy(&value, host + 4u * i, 4);
        assert(value == ((i % EXTENT) < 32 ? 0.25f : 1.0f));
    }
    assert(!ps5vk_depth_stencil_readback_detile(&image, S, host, sizeof(host),
                                                surface, sizeof(surface)));
    for (uint32_t i = 0; i < EXTENT * EXTENT; ++i)
        assert(host[i] == ((i % EXTENT) < 32 ? 0x5a : 0x17));
    assert(ps5vk_depth_stencil_readback_detile(&image, D | S, host, sizeof(host),
                                               surface, sizeof(surface)));
    assert(ps5vk_depth_stencil_readback_detile(&image, S, host, sizeof(host),
                                               surface, sizeof(surface) - 1));

    /* --- render-pass plan: per-aspect ops and layouts ---------------------- */
    VkAttachmentDescription a = {.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE};
    struct ps5vk_depth_stencil_layouts initial = {VK_IMAGE_LAYOUT_UNDEFINED, SA},
        reference = {DA, VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL}, final = {SRC, SA};
    struct ps5vk_depth_stencil_plan ds;
    assert(ps5vk_depth_stencil_attachment_plan(&a, a.format, &initial, &reference, &final, &ds) ==
           VK_SUCCESS);
    assert(ds.depth_clear && !ds.depth_load && ds.depth_store &&
           !ds.stencil_clear && ds.stencil_load && !ds.stencil_store);
    /* LOAD of an aspect with no contents. */
    initial.stencil = VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_depth_stencil_attachment_plan(&a, a.format, &initial, &reference, &final, &ds) !=
           VK_SUCCESS);
    initial.stencil = SA;
    /* A reference layout that names the other aspect, or a transfer layout. */
    reference.stencil = DA;
    assert(ps5vk_depth_stencil_attachment_plan(&a, a.format, &initial, &reference, &final, &ds) !=
           VK_SUCCESS);
    reference.stencil = SRC;
    assert(ps5vk_depth_stencil_attachment_plan(&a, a.format, &initial, &reference, &final, &ds) !=
           VK_SUCCESS);
    reference.stencil = DSA;   /* the combined layout means STENCIL_ATTACHMENT */
    assert(ps5vk_depth_stencil_attachment_plan(&a, a.format, &initial, &reference, &final, &ds) ==
           VK_SUCCESS);
    final.depth = VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_depth_stencil_attachment_plan(&a, a.format, &initial, &reference, &final, &ds) !=
           VK_SUCCESS);
    final.depth = DA;
    a.format = VK_FORMAT_D32_SFLOAT;
    assert(ps5vk_depth_stencil_attachment_plan(&a, VK_FORMAT_D32_SFLOAT, &initial, &reference,
                                               &final, &ds) != VK_SUCCESS);
    puts("Depth/stencil executor: per-aspect barriers, readback shapes, planes and plans hold (host only)");
    return 0;
}

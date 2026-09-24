/* VK_KHR_create_renderpass2: vkCreateRenderPass2KHR translates the version-2
 * description into the version-1 one and hands it to the single render pass
 * model. These host tests pin that the two entry points build the same object
 * from the same description, that everything the version-2 structures add is
 * validated (structure types, extension chains, input aspect masks, view masks
 * and view offsets), that unsupported extension structures are refused rather
 * than ignored, and that a refused call allocates nothing. */
#include "vk_render_pass.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned live_allocations, total_allocations;
static void *VKAPI_PTR counting_alloc(void *user, size_t size, size_t alignment,
                                      VkSystemAllocationScope scope)
{
    (void)user; (void)scope;
    /* malloc's alignment covers every object this model allocates. */
    assert(alignment <= 16);
    void *p = malloc(size);
    if (!p) return NULL;
    ++live_allocations; ++total_allocations;
    return p;
}
static void *VKAPI_PTR counting_realloc(void *user, void *original, size_t size,
                                        size_t alignment, VkSystemAllocationScope scope)
{
    (void)user; (void)original; (void)size; (void)alignment; (void)scope;
    return NULL;
}
static void VKAPI_PTR counting_free(void *user, void *memory)
{
    (void)user;
    if (!memory) return;
    --live_allocations;
    free(memory);
}
static const VkAllocationCallbacks counting = {
    .pfnAllocation = counting_alloc, .pfnReallocation = counting_realloc,
    .pfnFree = counting_free};

static int same_reference(VkAttachmentReference a, VkAttachmentReference b)
{
    return a.attachment == b.attachment && a.layout == b.layout;
}
/* One owned subpass, field by field: array entries past the declared counts
 * carry no meaning and are not compared. */
static void assert_same_subpass(const struct ps5vk_subpass *a, const struct ps5vk_subpass *b)
{
    assert(a->color_count == b->color_count && a->resolve_count == b->resolve_count);
    for (uint32_t i = 0; i < a->color_count; ++i)
        assert(same_reference(a->color[i], b->color[i]));
    for (uint32_t i = 0; i < a->resolve_count; ++i)
        assert(same_reference(a->resolve[i], b->resolve[i]));
    assert(same_reference(a->depth, b->depth));
    assert(a->input_first == b->input_first && a->input_count == b->input_count);
    assert(a->preserve_first == b->preserve_first && a->preserve_count == b->preserve_count);
}

/* The owned state two passes must agree on to be the same object. */
static void assert_same_pass(VkRenderPass a, VkRenderPass b)
{
    assert(a && b && a != b);
    assert(a->attachment_count == b->attachment_count);
    assert(a->subpass_count == b->subpass_count);
    assert(a->dependency_count == b->dependency_count);
    assert(a->input_count == b->input_count && a->preserve_count == b->preserve_count);
    assert(!memcmp(a->attachments, b->attachments,
                   a->attachment_count * sizeof(*a->attachments)));
    for (uint32_t i = 0; i < a->subpass_count; ++i)
        assert_same_subpass(&a->subpasses[i], &b->subpasses[i]);
    assert(!a->dependency_count || !memcmp(a->dependencies, b->dependencies,
                   a->dependency_count * sizeof(*a->dependencies)));
    assert(!a->input_count || !memcmp(a->inputs, b->inputs, a->input_count * sizeof(*a->inputs)));
    assert(!a->preserve_count ||
           !memcmp(a->preserves, b->preserves, a->preserve_count * sizeof(*a->preserves)));
    assert(!memcmp(&a->multiview, &b->multiview, sizeof(a->multiview)));
}

static VkAttachmentDescription2 attachment2(const VkAttachmentDescription *a)
{
    return (VkAttachmentDescription2){.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
        .flags = a->flags, .format = a->format, .samples = a->samples,
        .loadOp = a->loadOp, .storeOp = a->storeOp,
        .stencilLoadOp = a->stencilLoadOp, .stencilStoreOp = a->stencilStoreOp,
        .initialLayout = a->initialLayout, .finalLayout = a->finalLayout};
}
static VkAttachmentReference2 reference2(uint32_t attachment, VkImageLayout layout,
                                         VkImageAspectFlags aspect)
{
    return (VkAttachmentReference2){.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
        .attachment = attachment, .layout = layout, .aspectMask = aspect};
}
static VkSubpassDependency2 dependency2(const VkSubpassDependency *d, int32_t offset)
{
    return (VkSubpassDependency2){.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
        .srcSubpass = d->srcSubpass, .dstSubpass = d->dstSubpass,
        .srcStageMask = d->srcStageMask, .dstStageMask = d->dstStageMask,
        .srcAccessMask = d->srcAccessMask, .dstAccessMask = d->dstAccessMask,
        .dependencyFlags = d->dependencyFlags, .viewOffset = offset};
}

/* Assert that one version-2 create is refused with `expected`, writes a null
 * handle, creates no object and leaves no allocation behind. */
static void refused(struct VkDevice_T *d, const VkRenderPassCreateInfo2 *info, VkResult expected)
{
    const unsigned objects = d->graphics_objects, before = total_allocations;
    VkRenderPass pass = (VkRenderPass)(uintptr_t)1;
    VkResult rc = vkCreateRenderPass2KHR(d, info, &counting, &pass);
    if (rc != expected) fprintf(stderr, "expected %d got %d\n", (int)expected, (int)rc);
    assert(rc == expected);
    assert(pass == VK_NULL_HANDLE && d->graphics_objects == objects);
    assert(live_allocations == 0 && total_allocations == before);
}

/* Colour + depth, one subpass: the shape of the pinned renderpass2 simple
 * leaves. Both entry points build the same object, and the version-2 object is
 * owned (mutating the caller's structures changes nothing). */
static void single_subpass_equivalence(struct VkDevice_T *d)
{
    const VkAttachmentDescription attachments[2] = {
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color,
        .pDepthStencilAttachment = &depth};
    const VkSubpassDependency external = {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    const VkRenderPassCreateInfo info1 = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = attachments, .subpassCount = 1,
        .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &external};

    VkAttachmentDescription2 attachments2[2] = {attachment2(&attachments[0]),
                                                attachment2(&attachments[1])};
    /* The aspect mask of a colour or depth reference is ignored by Vulkan; a
     * junk value must not change the result. */
    VkAttachmentReference2 color2 = reference2(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               VK_IMAGE_ASPECT_METADATA_BIT);
    VkAttachmentReference2 depth2 = reference2(1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0);
    VkSubpassDescription2 sub2 = {.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color2,
        .pDepthStencilAttachment = &depth2};
    VkSubpassDependency2 external2 = dependency2(&external, 0);
    VkRenderPassCreateInfo2 info2 = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .attachmentCount = 2, .pAttachments = attachments2, .subpassCount = 1,
        .pSubpasses = &sub2, .dependencyCount = 1, .pDependencies = &external2};

    /* The extension gates the entry point: a device that did not enable it
     * refuses, even for a pass it would otherwise accept. */
    d->create_renderpass2_extension_enabled = VK_FALSE;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    d->create_renderpass2_extension_enabled = VK_TRUE;

    VkRenderPass one = VK_NULL_HANDLE, two = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(d, &info1, NULL, &one) == VK_SUCCESS);
    const unsigned objects = d->graphics_objects;
    assert(vkCreateRenderPass2KHR(d, &info2, &counting, &two) == VK_SUCCESS);
    assert(d->graphics_objects == objects + 1 && live_allocations == 1);
    assert_same_pass(one, two);
    assert(!two->multiview.present);
    /* Owned, not borrowed. */
    color2.attachment = 1; depth2.attachment = 0;
    attachments2[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    external2.dstSubpass = VK_SUBPASS_EXTERNAL;
    assert_same_pass(one, two);
    vkDestroyRenderPass(d, two, &counting);
    assert(live_allocations == 0 && d->graphics_objects == objects);
    vkDestroyRenderPass(d, one, NULL);

    /* A profile refusal of the version-1 path is the same refusal here. */
    color2.attachment = 0; depth2.attachment = 1;
    attachments2[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    external2.dstSubpass = 0;
    attachments2[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    attachments2[1].format = VK_FORMAT_D32_SFLOAT;
    color2.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    color2.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    info2.flags = 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.flags = 0;
    sub2.flags = 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    sub2.flags = 0;
    sub2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    sub2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

    /* Structure types: every level has one and each is checked. */
    info2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    info2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    attachments2[1].sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    attachments2[1].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    depth2.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    depth2.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    color2.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    color2.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    sub2.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    sub2.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    external2.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    external2.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;

    /* Extension structures this device does not implement are refused, not
     * ignored: separate stencil layouts on an attachment or a reference, a
     * depth/stencil resolve on a subpass, a synchronization2 barrier on a
     * dependency, and anything chained to the create info itself. */
    VkAttachmentDescriptionStencilLayout stencil_layouts = {
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT,
        .stencilInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .stencilFinalLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL};
    attachments2[1].pNext = &stencil_layouts;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    attachments2[1].pNext = NULL;
    VkAttachmentReferenceStencilLayout stencil_reference = {
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT,
        .stencilLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL};
    depth2.pNext = &stencil_reference;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    depth2.pNext = NULL;
    VkSubpassDescriptionDepthStencilResolve resolve = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
        .depthResolveMode = VK_RESOLVE_MODE_NONE, .stencilResolveMode = VK_RESOLVE_MODE_NONE};
    sub2.pNext = &resolve;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    sub2.pNext = NULL;
    VkMemoryBarrier2 barrier2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    external2.pNext = &barrier2;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    external2.pNext = NULL;
    VkRenderPassMultiviewCreateInfo v1_multiview = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    info2.pNext = &v1_multiview;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.pNext = NULL;

    /* Counts beyond the profile, and counts without arrays. */
    info2.subpassCount = PS5VK_MAX_SUBPASSES + 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.subpassCount = 1;
    info2.attachmentCount = PS5VK_MAX_ATTACHMENTS + 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.attachmentCount = 2;
    info2.dependencyCount = PS5VK_MAX_DEPENDENCIES + 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.dependencyCount = 1;
    info2.pDependencies = NULL;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.pDependencies = &external2;
    sub2.colorAttachmentCount = PS5VK_MAX_COLOR_ATTACHMENTS + 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    sub2.colorAttachmentCount = 1;
    sub2.pColorAttachments = NULL;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    sub2.pColorAttachments = &color2;
    const uint32_t preserve[3] = {0, 1, 0};
    sub2.preserveAttachmentCount = 3; sub2.pPreserveAttachments = preserve;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    sub2.preserveAttachmentCount = 0; sub2.pPreserveAttachments = NULL;
    info2.correlatedViewMaskCount = PS5VK_MAX_CORRELATION_MASKS + 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.correlatedViewMaskCount = 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.correlatedViewMaskCount = 0;

    /* Null arguments. */
    VkRenderPass untouched = (VkRenderPass)(uintptr_t)1;
    assert(vkCreateRenderPass2KHR(d, NULL, NULL, &untouched) == VK_ERROR_UNKNOWN &&
           untouched == VK_NULL_HANDLE);
    assert(vkCreateRenderPass2KHR(NULL, &info2, NULL, &untouched) == VK_ERROR_UNKNOWN);
    assert(vkCreateRenderPass2KHR(d, &info2, NULL, NULL) == VK_ERROR_UNKNOWN);
    assert(ps5vk_render_pass2_translate(NULL, NULL) == VK_ERROR_UNKNOWN);

    /* And the unmodified description still builds. */
    assert(vkCreateRenderPass2KHR(d, &info2, NULL, &two) == VK_SUCCESS);
    vkDestroyRenderPass(d, two, NULL);
}

/* Two subpasses: subpass 1 reads attachment 1 as an input attachment and
 * preserves nothing; a forward dependency links them. The version-2 input
 * reference carries an aspect mask, which must be exactly COLOR here. */
static void input_attachment_equivalence(struct VkDevice_T *d)
{
    const VkAttachmentDescription attachments[2] = {
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference inputs[2] = {
        {1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED}};
    const VkSubpassDescription subpasses[2] = {
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color},
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color,
         .inputAttachmentCount = 2, .pInputAttachments = inputs}};
    const VkSubpassDependency forward = {.srcSubpass = 0, .dstSubpass = 1,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};
    const VkRenderPassCreateInfo info1 = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = attachments, .subpassCount = 2,
        .pSubpasses = subpasses, .dependencyCount = 1, .pDependencies = &forward};

    const VkAttachmentDescription2 attachments2[2] = {attachment2(&attachments[0]),
                                                      attachment2(&attachments[1])};
    const VkAttachmentReference2 color2 =
        reference2(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
    /* The aspect of an UNUSED input reference is ignored. */
    VkAttachmentReference2 inputs2[2] = {
        reference2(1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT),
        reference2(VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED, 0)};
    VkSubpassDescription2 subpasses2[2] = {
        {.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
         .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color2},
        {.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
         .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color2,
         .inputAttachmentCount = 2, .pInputAttachments = inputs2}};
    VkSubpassDependency2 forward2 = dependency2(&forward, 0);
    VkRenderPassCreateInfo2 info2 = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .attachmentCount = 2, .pAttachments = attachments2, .subpassCount = 2,
        .pSubpasses = subpasses2, .dependencyCount = 1, .pDependencies = &forward2};

    VkRenderPass one = VK_NULL_HANDLE, two = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(d, &info1, NULL, &one) == VK_SUCCESS);
    assert(vkCreateRenderPass2KHR(d, &info2, &counting, &two) == VK_SUCCESS);
    assert_same_pass(one, two);
    VkImageLayout declared = VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_render_pass_attachment_layout(two, 1, 1, &declared) &&
           declared == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkDestroyRenderPass(d, two, &counting);
    vkDestroyRenderPass(d, one, NULL);
    assert(live_allocations == 0);

    /* Aspect masks of a real input reference (02800, 02801, 04563, 02525). */
    const VkImageAspectFlags invalid[] = {0, VK_IMAGE_ASPECT_METADATA_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_METADATA_BIT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_ASPECT_PLANE_0_BIT};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        inputs2[0].aspectMask = invalid[i];
        refused(d, &info2, VK_ERROR_UNKNOWN);
    }
    inputs2[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    /* An input naming an attachment outside the pass cannot be checked
     * against a format and is invalid. */
    inputs2[0].attachment = 2;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    inputs2[0].attachment = 1;
    /* The version-1 input-layout rules apply unchanged. */
    inputs2[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    inputs2[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    inputs2[1].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    refused(d, &info2, VK_ERROR_UNKNOWN);
    inputs2[1].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    subpasses2[1].inputAttachmentCount = PS5VK_MAX_INPUT_ATTACHMENTS + 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses2[1].inputAttachmentCount = 2;
    /* A view offset without a view-local dependency (03092). */
    forward2.viewOffset = 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    forward2.viewOffset = 0;
    /* A view-local dependency with every view mask zero (03057). */
    forward2.dependencyFlags |= VK_DEPENDENCY_VIEW_LOCAL_BIT;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    forward2.dependencyFlags = forward.dependencyFlags;
    assert(vkCreateRenderPass2KHR(d, &info2, NULL, &two) == VK_SUCCESS);
    vkDestroyRenderPass(d, two, NULL);
}

/* The aspect rule on its own, including the combined depth/stencil formats
 * the render pass profile does not serve yet: a strict subset there is valid
 * Vulkan that the owned model cannot express, so it is refused as unsupported
 * rather than as invalid. */
static void input_aspect_rule(void)
{
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_ASPECT_COLOR_BIT) == VK_SUCCESS);
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_ASPECT_DEPTH_BIT) == VK_SUCCESS);
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_D16_UNORM,
        VK_IMAGE_ASPECT_COLOR_BIT) == VK_ERROR_UNKNOWN);
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_ASPECT_STENCIL_BIT) == VK_ERROR_UNKNOWN);
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_S8_UINT,
        VK_IMAGE_ASPECT_STENCIL_BIT) == VK_SUCCESS);
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) == VK_SUCCESS);
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_D24_UNORM_S8_UINT,
        VK_IMAGE_ASPECT_DEPTH_BIT) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_D16_UNORM_S8_UINT,
        VK_IMAGE_ASPECT_STENCIL_BIT) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(ps5vk_render_pass_input_aspect_valid(VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_IMAGE_ASPECT_COLOR_BIT) == VK_ERROR_UNKNOWN);
}

/* Multiview through the version-2 structures: view masks on the subpasses,
 * view offsets on the dependencies and correlated masks on the pass. The
 * object is the one the version-1 path builds from the same masks chained as
 * VkRenderPassMultiviewCreateInfo, and every multiview obligation applies. */
static void multiview_equivalence(struct VkDevice_T *d)
{
    const VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpasses[2] = {
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color},
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color}};
    const VkSubpassDependency dependencies[2] = {
        {.srcSubpass = 0, .dstSubpass = 1,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .dependencyFlags = VK_DEPENDENCY_VIEW_LOCAL_BIT},
        {.srcSubpass = 1, .dstSubpass = 1,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
         .dependencyFlags = VK_DEPENDENCY_VIEW_LOCAL_BIT | VK_DEPENDENCY_BY_REGION_BIT}};
    const uint32_t masks[2] = {0x3u, 0x6u};
    const int32_t offsets[2] = {1, 0};
    const uint32_t correlated[2] = {0x1u, 0x6u};
    const VkRenderPassMultiviewCreateInfo mv = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .subpassCount = 2, .pViewMasks = masks, .dependencyCount = 2, .pViewOffsets = offsets,
        .correlationMaskCount = 2, .pCorrelationMasks = correlated};
    const VkRenderPassCreateInfo info1 = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = &mv, .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 2,
        .pSubpasses = subpasses, .dependencyCount = 2, .pDependencies = dependencies};

    const VkAttachmentDescription2 attachment2_ = attachment2(&attachment);
    const VkAttachmentReference2 color2 =
        reference2(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
    VkSubpassDescription2 subpasses2[2];
    for (unsigned i = 0; i < 2; ++i)
        subpasses2[i] = (VkSubpassDescription2){.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .viewMask = masks[i],
            .colorAttachmentCount = 1, .pColorAttachments = &color2};
    VkSubpassDependency2 dependencies2[2] = {dependency2(&dependencies[0], offsets[0]),
                                             dependency2(&dependencies[1], offsets[1])};
    uint32_t correlated2[2] = {correlated[0], correlated[1]};
    VkRenderPassCreateInfo2 info2 = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .attachmentCount = 1, .pAttachments = &attachment2_, .subpassCount = 2,
        .pSubpasses = subpasses2, .dependencyCount = 2, .pDependencies = dependencies2,
        .correlatedViewMaskCount = 2, .pCorrelatedViewMasks = correlated2};

    /* Without the multiview feature every view mask must be zero (06558). */
    const uint32_t saved = d->enabled_features;
    d->enabled_features &= ~(uint32_t)PS5VK_FEATURE_MULTIVIEW;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    d->enabled_features |= PS5VK_FEATURE_MULTIVIEW;

    VkRenderPass one = VK_NULL_HANDLE, two = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(d, &info1, NULL, &one) == VK_SUCCESS);
    assert(vkCreateRenderPass2KHR(d, &info2, &counting, &two) == VK_SUCCESS);
    assert_same_pass(one, two);
    assert(two->multiview.present && two->multiview.view_masks[0] == 0x3u &&
           two->multiview.view_masks[1] == 0x6u && two->multiview.view_offsets[0] == 1 &&
           two->multiview.correlation_mask_count == 2 &&
           two->multiview.correlation_masks[1] == 0x6u);
    /* Owned: the caller's masks can change afterwards. */
    subpasses2[0].viewMask = 0; correlated2[1] = 0;
    assert(two->multiview.view_masks[0] == 0x3u && two->multiview.correlation_masks[1] == 0x6u);
    subpasses2[0].viewMask = masks[0]; correlated2[1] = correlated[1];
    vkDestroyRenderPass(d, two, &counting);
    vkDestroyRenderPass(d, one, NULL);
    assert(live_allocations == 0);

    /* All-or-nothing view masks (03058). */
    subpasses2[1].viewMask = 0;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    /* Every mask zero: then neither correlated masks (03059) nor view-local
     * dependencies (03057) may be declared. */
    subpasses2[0].viewMask = 0;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.correlatedViewMaskCount = 0;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    info2.correlatedViewMaskCount = 2;
    subpasses2[0].viewMask = masks[0]; subpasses2[1].viewMask = masks[1];
    /* The most significant view must be below maxMultiviewViewCount (06706). */
    subpasses2[1].viewMask = 1u << PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses2[1].viewMask = masks[1];
    /* Correlated masks must be disjoint (03056). */
    correlated2[1] = 0x3u;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    correlated2[1] = correlated[1];
    /* A non-zero offset on a self-dependency (02530), and an offset on a
     * dependency that is not view-local (03092). */
    dependencies2[1].viewOffset = 1;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    dependencies2[1].viewOffset = 0;
    dependencies2[0].dependencyFlags = 0;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    dependencies2[0].dependencyFlags = VK_DEPENDENCY_VIEW_LOCAL_BIT;
    /* A self-dependency of a multi-view subpass must be view-local (03060);
     * the profile additionally requires it to be by-region. */
    dependencies2[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    dependencies2[1].dependencyFlags = dependencies[1].dependencyFlags;
    /* A view-local dependency with an external side (03090/03091). */
    dependencies2[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    refused(d, &info2, VK_ERROR_FEATURE_NOT_PRESENT);
    dependencies2[0].srcSubpass = 0;
    assert(vkCreateRenderPass2KHR(d, &info2, NULL, &two) == VK_SUCCESS);
    vkDestroyRenderPass(d, two, NULL);
    d->enabled_features = saved;
}

/* VkRenderPassInputAttachmentAspectCreateInfo (VK_KHR_maintenance2) on the
 * version-1 path: accepted once, only on a device that enabled the extension,
 * for input references that exist, under the same aspect rule the version-2
 * reference obeys. */
static void input_aspect_create_info(struct VkDevice_T *d)
{
    const VkAttachmentDescription attachments[2] = {
        {.format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference inputs[2] = {
        {1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED}};
    const VkSubpassDescription subpasses[2] = {
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color},
        {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount = 1, .pColorAttachments = &color,
         .inputAttachmentCount = 2, .pInputAttachments = inputs}};
    VkInputAttachmentAspectReference aspects[2] = {
        {.subpass = 1, .inputAttachmentIndex = 0, .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
        /* The aspect of an UNUSED input reference is not checked. */
        {.subpass = 1, .inputAttachmentIndex = 1, .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT}};
    VkRenderPassInputAttachmentAspectCreateInfo aspect_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,
        .aspectReferenceCount = 2, .pAspectReferences = aspects};
    VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = &aspect_info, .attachmentCount = 2, .pAttachments = attachments,
        .subpassCount = 2, .pSubpasses = subpasses};
    const unsigned objects = d->graphics_objects;
    VkRenderPass pass = (VkRenderPass)(uintptr_t)1, plain = VK_NULL_HANDLE;

    d->maintenance2_extension_enabled = VK_FALSE;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT &&
           !pass && d->graphics_objects == objects);
    d->maintenance2_extension_enabled = VK_TRUE;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
    /* The accepted mask is every aspect, so the object is the plain one. */
    info.pNext = NULL;
    assert(vkCreateRenderPass(d, &info, NULL, &plain) == VK_SUCCESS);
    assert_same_pass(pass, plain);
    vkDestroyRenderPass(d, plain, NULL);
    vkDestroyRenderPass(d, pass, NULL);
    info.pNext = &aspect_info;

    struct { uint32_t subpass, index; VkImageAspectFlags aspect; VkResult expected; } bad[] = {
        {2, 0, VK_IMAGE_ASPECT_COLOR_BIT, VK_ERROR_UNKNOWN},    /* 01926 */
        {0, 0, VK_IMAGE_ASPECT_COLOR_BIT, VK_ERROR_UNKNOWN},    /* 01927 */
        {1, 2, VK_IMAGE_ASPECT_COLOR_BIT, VK_ERROR_UNKNOWN},    /* 01927 */
        {1, 0, VK_IMAGE_ASPECT_DEPTH_BIT, VK_ERROR_UNKNOWN},    /* 01963 */
        {1, 0, 0, VK_ERROR_UNKNOWN},
        {1, 0, VK_IMAGE_ASPECT_METADATA_BIT, VK_ERROR_UNKNOWN}};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        aspects[0] = (VkInputAttachmentAspectReference){bad[i].subpass, bad[i].index,
                                                        bad[i].aspect};
        pass = (VkRenderPass)(uintptr_t)1;
        assert(vkCreateRenderPass(d, &info, NULL, &pass) == bad[i].expected &&
               !pass && d->graphics_objects == objects);
    }
    aspects[0] = (VkInputAttachmentAspectReference){1, 0, VK_IMAGE_ASPECT_COLOR_BIT};
    aspect_info.aspectReferenceCount = 0;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN && !pass);
    aspect_info.aspectReferenceCount = 2;
    /* A second copy of the structure. */
    VkRenderPassInputAttachmentAspectCreateInfo second = aspect_info;
    second.pNext = &aspect_info;
    info.pNext = &second;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT && !pass);
    d->maintenance2_extension_enabled = VK_FALSE;
    assert(d->graphics_objects == objects);
}

int main(void)
{
    struct VkDevice_T d = {0};
    d.graphics_enabled = VK_TRUE;
    d.create_renderpass2_extension_enabled = VK_TRUE;
    input_aspect_rule();
    single_subpass_equivalence(&d);
    input_attachment_equivalence(&d);
    multiview_equivalence(&d);
    input_aspect_create_info(&d);
    assert(d.graphics_objects == 0 && d.lifetime_errors == 0 && live_allocations == 0);
    puts("vk render pass 2 tests passed");
    return 0;
}

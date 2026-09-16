/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The bounded one-input subpass-read admission rule: exactly the shape the
 * native oracle executes is admitted, and every neighbouring shape - a
 * combined record's width, a vertex-visible declaration, a missing or UNUSED
 * input reference, another view or layer, another layout, a missing or
 * mismatched dependency, a second input binding and a read in subpass 0 - is
 * refused before any packet could be emitted. */
#include "input_attachment_gate.h"
#include "descriptor_table_layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { GATE_SET = 0, GATE_BINDING = 0, GATE_ELEMENT = 0, GATE_SUBPASS = 1 };

struct gate_fixture {
    struct VkDevice_T device, foreign_device;
    struct VkImageView_T attachment_view, other_view;
    struct VkImage_T attachment_image, other_image;
    struct VkDescriptorPool_T pool;
    struct VkDescriptorSet_T set;
    struct ps5vk_binding binding;
    struct ps5vk_runtime_draw_abi abi;
    struct VkFramebuffer_T framebuffer;
    struct VkRenderPass_T pass;
    struct ps5vk_subpass subpasses[PS5VK_MAX_SUBPASSES];
    VkAttachmentReference inputs[1];
    VkSubpassDependency dependencies[PS5VK_MAX_DEPENDENCIES];
    VkAttachmentDescription attachments[1];
};

/* The exact measured shape: a two-subpass pass over one attachment that
 * subpass 0 writes as colour and subpass 1 both reads (pInputAttachments[0])
 * and writes, in GENERAL, with the forward BY_REGION dependency between them. */
static void fixture_init(struct gate_fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->attachment_image.device = &f->device;
    f->attachment_image.info.format = VK_FORMAT_R8G8B8A8_UNORM;
    f->attachment_image.info.imageType = VK_IMAGE_TYPE_2D;
    f->attachment_image.info.extent = (VkExtent3D){16, 16, 1};
    f->attachment_image.info.arrayLayers = 6;
    f->attachment_image.info.mipLevels = 1;
    f->attachment_image.info.samples = VK_SAMPLE_COUNT_1_BIT;
    f->attachment_image.info.tiling = VK_IMAGE_TILING_OPTIMAL;
    f->attachment_image.info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    f->attachment_view.device = &f->device;
    f->attachment_view.image = &f->attachment_image;
    f->attachment_view.view_type = VK_IMAGE_VIEW_TYPE_2D;
    f->attachment_view.format = f->attachment_image.info.format;
    f->attachment_view.range = (VkImageSubresourceRange){
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f->other_view = f->attachment_view;
    f->other_view.image = &f->other_image;

    f->pool.device = &f->device;
    f->set.pool = &f->pool;
    f->set.generation = 3;
    f->set.defined[GATE_ELEMENT] = VK_TRUE;
    f->set.images[GATE_ELEMENT] = (VkDescriptorImageInfo){
        VK_NULL_HANDLE, &f->attachment_view, VK_IMAGE_LAYOUT_GENERAL};
    f->set.image_resources[GATE_ELEMENT] = &f->attachment_image;
    f->binding.count = 1;
    f->binding.first = 0;
    f->binding.stages = VK_SHADER_STAGE_FRAGMENT_BIT;
    f->set.signature.binding[GATE_BINDING] = f->binding;
    f->set.signature.type[GATE_BINDING] = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    f->set.signature.count = 1;

    f->abi.enabled = 1;
    f->abi.fragment_descriptor_valid[GATE_SET] = VK_TRUE;
    f->abi.fragment_used_bindings[GATE_SET] = UINT64_C(1) << GATE_BINDING;

    f->framebuffer.device = &f->device;
    f->framebuffer.width = f->framebuffer.height = 64;
    f->framebuffer.attachment_count = 1;
    f->framebuffer.attachments[0] = &f->attachment_view;
    f->framebuffer.color_attachment = 0;
    f->framebuffer.depth_attachment = VK_ATTACHMENT_UNUSED;

    f->attachments[0] = (VkAttachmentDescription){
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    f->subpasses[0].color = (VkAttachmentReference){0, VK_IMAGE_LAYOUT_GENERAL};
    f->subpasses[0].depth = (VkAttachmentReference){VK_ATTACHMENT_UNUSED,
        VK_IMAGE_LAYOUT_UNDEFINED};
    f->subpasses[1] = f->subpasses[0];
    f->inputs[0] = (VkAttachmentReference){0, VK_IMAGE_LAYOUT_GENERAL};
    f->subpasses[1].input_first = 0;
    f->subpasses[1].input_count = 1;
    f->dependencies[0] = (VkSubpassDependency){
        .srcSubpass = 0, .dstSubpass = GATE_SUBPASS,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};
    f->pass.device = &f->device;
    f->pass.attachment_count = 1;
    f->pass.subpass_count = PS5VK_MAX_SUBPASSES;
    f->pass.dependency_count = 1;
    f->pass.attachments = f->attachments;
    f->pass.subpasses = f->subpasses;
    f->pass.dependencies = f->dependencies;
    f->pass.inputs = f->inputs;
    f->pass.input_count = 1;
}

static VkResult gate(struct gate_fixture *f)
{
    return ps5vk_input_attachment_gate((VkDevice)&f->device, &f->pass, GATE_SUBPASS,
        &f->framebuffer, (VkDescriptorSet)&f->set,
        &f->set.signature.binding[GATE_BINDING], GATE_BINDING,
        VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, GATE_ELEMENT, 1, &f->abi, GATE_SET);
}

/* A mutation has to be able to write through the fixture's own pointers, so
 * every case runs on a copy that points at ITS OWN arrays and objects rather
 * than at the reference fixture's. Otherwise one mutation would quietly edit
 * the baseline every later case is compared with. */
static void fixture_clone(struct gate_fixture *out, const struct gate_fixture *in)
{
    memcpy(out, in, sizeof(*out));
    out->attachment_image.device = &out->device;
    out->other_image.device = &out->device;
    out->attachment_view.device = &out->device;
    out->attachment_view.image = &out->attachment_image;
    out->other_view.device = &out->device;
    out->other_view.image = &out->other_image;
    out->pool.device = &out->device;
    out->set.pool = &out->pool;
    out->pass.device = &out->device;
    out->pass.attachments = out->attachments;
    out->pass.subpasses = out->subpasses;
    out->pass.dependencies = out->dependencies;
    out->pass.inputs = out->inputs;
    out->framebuffer.device = &out->device;
    out->framebuffer.attachments[0] = out->framebuffer.attachments[0] == &in->attachment_view
        ? &out->attachment_view : &out->other_view;
    out->set.images[GATE_ELEMENT].imageView =
        out->set.images[GATE_ELEMENT].imageView == &in->attachment_view
        ? &out->attachment_view : (out->set.images[GATE_ELEMENT].imageView == &in->other_view
            ? &out->other_view : out->set.images[GATE_ELEMENT].imageView);
    out->set.image_resources[GATE_ELEMENT] =
        out->set.image_resources[GATE_ELEMENT] == &in->attachment_image
        ? &out->attachment_image : &out->other_image;
    out->set.signature.binding[GATE_BINDING] = out->binding;
}

int main(void)
{
    struct gate_fixture base;
    fixture_init(&base);
    /* The measured shape is admitted, and its record is the resource-only eight
     * DWORD image record - never a combined T#/S# pair. */
    assert(gate(&base) == VK_SUCCESS);
    assert(ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) == 32);
    assert(ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) == 48);

    /* One mutation per refusal: the fixture is rebuilt each time and exactly
     * one thing about it changes, so the case says what it refuses. */
    struct gate_fixture f;
#define MUTATE(statement) do { fixture_clone(&f, &base); statement; \
        assert(gate(&f) == VK_ERROR_FEATURE_NOT_PRESENT); } while (0)
    MUTATE(f.set.signature.binding[GATE_BINDING].stages = VK_SHADER_STAGE_VERTEX_BIT);
    MUTATE(f.set.signature.binding[GATE_BINDING].stages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    MUTATE(f.set.signature.binding[GATE_BINDING].stages = VK_SHADER_STAGE_ALL);
    MUTATE(f.set.signature.binding[GATE_BINDING].stages = 0);
    /* One element, not an array: a descriptorCount of two would be a second
     * attachment this profile never sized or measured. */
    MUTATE(f.set.signature.binding[GATE_BINDING].count = 2);
    /* Vertex-only delivery, an unusable ABI and a binding no compiled fragment
     * stage dereferences are all refused. */
    MUTATE(f.abi.fragment_descriptor_valid[GATE_SET] = VK_FALSE);
    MUTATE(f.abi.vertex_descriptor_valid[GATE_SET] = VK_TRUE);
    MUTATE(f.abi.fragment_used_bindings[GATE_SET] = 0);
    MUTATE(f.abi.enabled = 0);
    MUTATE(f.pass.attachment_count = 0);
    MUTATE(f.pass.subpass_count = 1);
    MUTATE(f.pass.subpasses[GATE_SUBPASS].input_count = 0);
    MUTATE(f.pass.subpasses[GATE_SUBPASS].input_count = 2);
    /* The subpass's slice of the pass's own input array is bounds-checked
     * before it is indexed, so a malformed pass is never read at all. */
    MUTATE(f.pass.subpasses[GATE_SUBPASS].input_first = 1);
    MUTATE(f.pass.input_count = 0);
    MUTATE(f.pass.inputs[0].attachment = VK_ATTACHMENT_UNUSED);
    MUTATE(f.pass.inputs[0].attachment = 1);
    MUTATE(f.pass.inputs[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    MUTATE(f.pass.inputs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    /* The descriptor must BE that framebuffer view, recorded in GENERAL. */
    MUTATE(f.framebuffer.attachment_count = 0);
    MUTATE(f.framebuffer.attachments[0] = NULL);
    MUTATE(f.framebuffer.attachments[0] = &f.other_view);
    MUTATE(f.set.image_resources[GATE_ELEMENT] = &f.other_image);
    MUTATE(f.set.images[GATE_ELEMENT].imageView = &f.other_view);
    MUTATE(f.set.images[GATE_ELEMENT].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    MUTATE(f.set.images[GATE_ELEMENT].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    MUTATE(f.set.defined[GATE_ELEMENT] = VK_FALSE);
    /* The transition that makes the pixels visible has to be in the pass. */
    MUTATE(f.pass.dependency_count = 0);
    MUTATE(f.dependencies[0].dstSubpass = 0);
    MUTATE(f.dependencies[0].srcStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    MUTATE(f.dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    MUTATE(f.dependencies[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT);
    MUTATE(f.dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    MUTATE(f.dependencies[0].dependencyFlags = 0);
    MUTATE(f.dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    MUTATE(f.dependencies[0].dstAccessMask =
        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    /* The measured dependency is BY_REGION and nothing else: an extra flag is a
     * different transition than the one that was witnessed. */
    MUTATE(f.dependencies[0].dependencyFlags =
        VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT);
    /* The promoted resource shape itself: the exact image and the exact
     * layer-0 view, one field at a time. */
    MUTATE(f.attachment_image.info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    MUTATE(f.attachment_image.info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT);
    MUTATE(f.attachment_image.info.usage &=
        ~(VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    MUTATE(f.attachment_image.info.arrayLayers = 5);
    MUTATE(f.attachment_image.info.arrayLayers = 7);
    MUTATE(f.attachment_image.info.format = VK_FORMAT_B8G8R8A8_UNORM);
    MUTATE(f.attachment_image.info.imageType = VK_IMAGE_TYPE_3D);
    MUTATE(f.attachment_image.info.samples = VK_SAMPLE_COUNT_2_BIT);
    MUTATE(f.attachment_image.info.mipLevels = 2);
    MUTATE(f.attachment_image.info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    MUTATE(f.attachment_image.info.tiling = VK_IMAGE_TILING_LINEAR);
    MUTATE(f.attachment_image.info.extent.depth = 2);
    MUTATE(f.attachment_view.range.baseArrayLayer = 1);
    MUTATE(f.attachment_view.range.layerCount = 6);
    MUTATE(f.attachment_view.range.baseMipLevel = 1);
    MUTATE(f.attachment_view.range.levelCount = 2);
    MUTATE(f.attachment_view.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT);
    MUTATE(f.attachment_view.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    MUTATE(f.attachment_view.format = VK_FORMAT_B8G8R8A8_UNORM);
    MUTATE(f.attachment_view.device = &f.foreign_device);
    MUTATE(f.attachment_image.device = &f.foreign_device);
#undef MUTATE

    /* A read in the subpass that produced the pixels has nothing to read. */
    fixture_clone(&f, &base);
    assert(ps5vk_input_attachment_gate((VkDevice)&f.device, &f.pass, 0, &f.framebuffer,
        (VkDescriptorSet)&f.set, &f.binding, GATE_BINDING,
        VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, GATE_ELEMENT, 1, &f.abi, GATE_SET) ==
        VK_ERROR_FEATURE_NOT_PRESENT);
    /* ...but with two input bindings the whole table is refused, before the
     * per-element rule is even asked. */
    for (uint32_t declared = 0; declared <= 2; ++declared) {
        fixture_clone(&f, &base);
        VkResult rc = ps5vk_input_attachment_gate((VkDevice)&f.device, &f.pass, GATE_SUBPASS,
            &f.framebuffer, (VkDescriptorSet)&f.set, &f.binding, GATE_BINDING,
            VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, GATE_ELEMENT, declared, &f.abi, GATE_SET);
        assert(rc == (declared == 1 ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT));
    }
    /* A combined pair, a buffer and a sampled image are not this role at all:
     * asking the gate about them is a caller error, not an admitted shape. */
    assert(ps5vk_input_attachment_gate((VkDevice)&base.device, &base.pass, GATE_SUBPASS,
        &base.framebuffer, (VkDescriptorSet)&base.set, &base.binding, GATE_BINDING,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GATE_ELEMENT, 1, &base.abi, GATE_SET) ==
        VK_ERROR_UNKNOWN);
    assert(ps5vk_input_attachment_gate((VkDevice)&base.device, &base.pass, GATE_SUBPASS,
        &base.framebuffer, (VkDescriptorSet)&base.set, &base.binding, GATE_BINDING,
        VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, GATE_ELEMENT, 1, NULL, GATE_SET) ==
        VK_ERROR_UNKNOWN);
    assert(ps5vk_input_attachment_gate(NULL, &base.pass, GATE_SUBPASS, &base.framebuffer,
        (VkDescriptorSet)&base.set, &base.binding, GATE_BINDING,
        VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, GATE_ELEMENT, 1, &base.abi, GATE_SET) ==
        VK_ERROR_UNKNOWN);
    puts("Input-attachment gate: one bounded subpass read, every other shape refused");
    return 0;
}

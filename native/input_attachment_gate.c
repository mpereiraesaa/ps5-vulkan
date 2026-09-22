/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The bounded one-input subpass-read profile, as one pure decision over the
 * objects the driver already holds. See input_attachment_gate.h for the shape
 * and for why the real objects are the input. */
#include "input_attachment_gate.h"
#include "descriptor_table_layout.h"

/* The one forward dependency this profile accepts: subpass 0's colour write
 * visible to the later subpass's fragment input-attachment read, in the same
 * BY_REGION scope, and nothing else on either side. A dependency that names a
 * different subpass pair, misses a stage, an access or the region scope, or
 * carries extra bits the profile does not model, is refused rather than
 * accepted as "close enough" - the readback oracle depends on this transition
 * existing exactly. */
static int forward_dependency(const VkRenderPass pass, uint32_t subpass)
{
    for (uint32_t i = 0; i < pass->dependency_count; ++i) {
        const VkSubpassDependency *dependency = &pass->dependencies[i];
        if (dependency->srcSubpass != 0 || dependency->dstSubpass != subpass) continue;
        if (dependency->srcStageMask == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT &&
            dependency->dstStageMask == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT &&
            dependency->srcAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
            dependency->dstAccessMask == VK_ACCESS_INPUT_ATTACHMENT_READ_BIT &&
            dependency->dependencyFlags == VK_DEPENDENCY_BY_REGION_BIT)
            return 1;
    }
    return 0;
}

/* The two resources this profile has measured.
 *
 * The promoted multiview attachment: one RGBA8 2D six-layer attachment created
 * for the colour/transfer/input-attachment roles, read through a single-layer,
 * single-level colour view of layer 0. And, since DXVK262-T06's sample-rate
 * line, the multisampled colour attachment the pinned oracle reads once per
 * sample: the same RGBA8 2D shape at a served sample count, single-layer and
 * single-mip, created for the colour, readback-source and input-attachment
 * roles only. Both are read through the same 2D colour view of layer zero on
 * the same device; a descriptor naming anything else has not been witnessed,
 * so it is refused rather than read as if it had been. */
static int promoted_resource(VkDevice device, VkImage image, VkImageView view)
{
    if (!image || !view || image->device != device || view->device != device ||
        view->image != image) return 0;
    const VkImageCreateInfo *info = &image->info;
    const VkImageUsageFlags multiview_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags multisample_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    const int multisampled = ps5vk_sample_count_implemented(info->samples) &&
        info->samples != VK_SAMPLE_COUNT_1_BIT;
    if (info->format != VK_FORMAT_R8G8B8A8_UNORM || info->imageType != VK_IMAGE_TYPE_2D ||
        info->mipLevels != 1u || info->flags ||
        info->tiling != VK_IMAGE_TILING_OPTIMAL || info->extent.depth != 1u) return 0;
    if (multisampled) {
        if (info->arrayLayers != 1u || info->usage != multisample_usage) return 0;
    } else if (info->samples != VK_SAMPLE_COUNT_1_BIT ||
               info->arrayLayers != (uint32_t)PS5VK_INPUT_ATTACHMENT_LAYER_COUNT ||
               info->usage != multiview_usage) return 0;
    const VkImageSubresourceRange *range = &view->range;
    if (view->view_type != VK_IMAGE_VIEW_TYPE_2D || view->format != info->format ||
        range->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || range->baseMipLevel ||
        range->levelCount != 1u || range->baseArrayLayer || range->layerCount != 1u)
        return 0;
    return 1;
}

VkResult ps5vk_input_attachment_gate(VkDevice device, VkRenderPass pass, uint32_t subpass,
    VkFramebuffer framebuffer, VkDescriptorSet set, const struct ps5vk_binding *binding,
    uint32_t binding_index, VkDescriptorType type, unsigned element_index,
    uint32_t input_binding_count, const struct ps5vk_runtime_draw_abi *abi,
    unsigned set_index)
{
    /* A caller that hands this gate an incomplete request has a bug rather
     * than an unsupported shape. */
    if (!device || !pass || !framebuffer || !set || !binding || !abi ||
        set_index >= PS5VK_MAX_SETS || binding_index >= PS5VK_MAX_BINDINGS ||
        element_index >= PS5VK_MAX_DESCRIPTORS) return VK_ERROR_UNKNOWN;
    if (type != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) return VK_ERROR_UNKNOWN;
    /* The role is fragment-visible resource-only image data and the compiled
     * fragment stage has to be the one that names it. A vertex-visible
     * declaration, a set only the vertex stage was given, or a binding no
     * compiled fragment stage dereferences is not this profile. */
    if (binding->stages != VK_SHADER_STAGE_FRAGMENT_BIT) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* One element, not an array: the measured profile reads one attachment, and
     * a second element would be a second descriptor this gate never sized. */
    if (binding->count != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!abi->enabled || !abi->fragment_descriptor_valid[set_index] ||
        abi->vertex_descriptor_valid[set_index] ||
        !(abi->fragment_used_bindings[set_index] & (UINT64_C(1) << binding_index)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Exactly one input binding in the whole table: this profile has measured
     * one attachment read, not an array of them. */
    if (input_binding_count != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* An input attachment is read by a LATER subpass; the subpass that produced
     * the pixels has nothing to read yet. */
    if (subpass == 0 || subpass >= pass->subpass_count) return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct ps5vk_subpass *stage = &pass->subpasses[subpass];
    /* Missing, UNUSED or multiplied input references are all refused: the read
     * this profile serves is pInputAttachments[0] of that subpass and nothing
     * else. The subpass's slice is bounds-checked against the pass's own array
     * before it is indexed, so a malformed pass cannot be read at all. */
    if (stage->input_count != 1 || stage->input_first >= pass->input_count ||
        pass->input_count - stage->input_first < stage->input_count)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkAttachmentReference *reference = &pass->inputs[stage->input_first];
    if (reference->attachment == VK_ATTACHMENT_UNUSED) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (reference->attachment >= pass->attachment_count ||
        reference->attachment >= framebuffer->attachment_count ||
        !framebuffer->attachments[reference->attachment])
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* A subpass input reference is a read layout; the colour- and
     * depth-attachment layouts are refused at pass creation for the same
     * reason, and GENERAL is what this profile's boundary transition leaves the
     * attachment in. */
    if (reference->layout != VK_IMAGE_LAYOUT_GENERAL) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!set->defined[element_index]) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* The descriptor must BE the framebuffer view of that reference - not
     * another view of the same image, not another layer - and it must be
     * recorded in GENERAL, the layout the acquire at the subpass boundary
     * really leaves behind. */
    if (!set->images[element_index].imageView ||
        set->images[element_index].imageView != framebuffer->attachments[reference->attachment] ||
        set->image_resources[element_index] !=
            framebuffer->attachments[reference->attachment]->image)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!promoted_resource(device, set->image_resources[element_index],
            set->images[element_index].imageView))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (set->images[element_index].imageLayout != VK_IMAGE_LAYOUT_GENERAL)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* No sampler words: the record this role occupies is the eight DWORD
     * resource-only image record, so a table that sized it as a combined pair
     * can never reach execution through here. */
    if (ps5vk_descriptor_record_bytes(type) != 32) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* The transition that makes the pixels visible is the boundary barrier
     * this executor emits for every subpass that reads an input attachment
     * (PS5VK_COLOR_TO_TEXTURE_BARRIER around the subpass change). An explicit
     * forward dependency is what the multiview witness declared and is still
     * accepted here, but it is not required for the read to be ordered: Vulkan
     * gives an attachment read by a later subpass its implicit dependency, and
     * the executor's own barrier carries it either way. */
    (void)forward_dependency;
    return VK_SUCCESS;
}

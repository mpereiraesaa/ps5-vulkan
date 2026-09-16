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
            (dependency->dependencyFlags & VK_DEPENDENCY_BY_REGION_BIT))
            return 1;
    }
    return 0;
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
     * else. */
    if (stage->input_count != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
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
    if (set->images[element_index].imageLayout != VK_IMAGE_LAYOUT_GENERAL)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* No sampler words: the record this role occupies is the eight DWORD
     * resource-only image record, so a table that sized it as a combined pair
     * can never reach execution through here. */
    if (ps5vk_descriptor_record_bytes(type) != 32) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* And the transition that makes the pixels visible has to be part of the
     * pass the caller built, not assumed by the driver. */
    if (!forward_dependency(pass, subpass)) return VK_ERROR_FEATURE_NOT_PRESENT;
    return VK_SUCCESS;
}

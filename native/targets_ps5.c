#include "targets_ps5.h"
#include "depth_layout.h"
#include "color_attachment_contract.h"
#include <string.h>

/* The colour target's sample geometry lives in CB_COLOR0_ATTRIB (context
 * offset 0x31d), whose NUM_SAMPLES/NUM_FRAGMENTS fields the shared builder
 * clears. A single-sample target keeps those zeroes - byte for byte what this
 * driver emitted before the field existed - and a multisampled one states log2
 * of its count in both, which is the non-EQAA shape this contract serves. The
 * FMASK base stays zero: nothing here allocates coverage storage, so pointing
 * the target at one would name memory that does not exist. */
static VkResult color_target_samples(ps5_agc_register registers[PS5_COLOR_REGISTER_COUNT],
    VkSampleCountFlagBits samples)
{
    if (!ps5vk_sample_count_implemented(samples)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    registers[3].value = (registers[3].value & ~PS5VK_COLOR_ATTRIB_SAMPLE_FIELDS_MASK) |
        ps5vk_color_attrib_sample_fields(samples);
    return VK_SUCCESS;
}

VkResult ps5vk_native_target(VkDevice d, VkImageView view,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_target_registers *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    /* The whole-image target: mip zero and layer zero of the image, which is
     * what the base target of a one-layer view has always been. A layered
     * attachment view (baseArrayLayer zero, layerCount = the views its subpass
     * renders) prepares exactly as the single-layer one did - the per-view
     * layer selection happens when the draw is emitted for a view, and a view
     * that does not start at layer zero still has no whole-image target. */
    if (!d || !view || view->device != d || view->range.baseMipLevel ||
        view->range.levelCount != 1 || view->range.baseArrayLayer || !view->range.layerCount)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImage image = view->image;
    void *address; VkDeviceSize bytes;
    VkResult rc = ps5vk_image_span(d, image, &address, &bytes);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements required;
    rc = ps5vk_native_image_requirements(d, &image->info, &required);
    if (rc != VK_SUCCESS) return rc;
    uintptr_t base = (uintptr_t)address;
    if (bytes < required.size || base % required.alignment || !base ||
        base >= (UINT64_C(1) << 48) || bytes > (UINT64_C(1) << 48) - base) return VK_ERROR_UNKNOWN;
    struct ps5vk_target_registers result = {0};
    uint32_t width = image->info.extent.width, height = image->info.extent.height;
    if (view->format == VK_FORMAT_D32_SFLOAT || view->format == VK_FORMAT_D16_UNORM) {
        if (!(image->info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return VK_ERROR_UNKNOWN;
        /* No multisampled depth target exists on this path yet, so a depth
         * surface this profile creates is single-sample; the colour role is
         * the only one that carries a count (DXVK262-T06). */
        if (image->info.samples != VK_SAMPLE_COUNT_1_BIT) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (ps5_depth_build_d32_no_htile(result.registers, base, width, height)) return VK_ERROR_UNKNOWN;
        /* Public GFX10 DB_Z_INFO.FORMAT: Z_16=1, Z_32_FLOAT=3. Keep the
         * measured no-HTILE target plan and alter only its documented format
         * field for the single 128x128 diagnostic shape. */
        if (view->format == VK_FORMAT_D16_UNORM)
            result.registers[20].value = (result.registers[20].value & ~UINT32_C(3)) | UINT32_C(1);
        result.count = PS5_DEPTH_REGISTER_COUNT;
    } else if (view->format == VK_FORMAT_B8G8R8A8_UNORM ||
               view->format == VK_FORMAT_R8G8B8A8_UNORM ||
               ps5vk_color_target_integer_served(view->format)) {
        if (!(image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return VK_ERROR_UNKNOWN;
        if (ps5_color_build_target(result.registers, color_defaults, base, width, height)) return VK_ERROR_UNKNOWN;
        /* Gears' generic builder selects COMP_SWAP=STD (RGBA byte order).
         * Vulkan BGRA requires ZYXW / SWAP_ALT=1 in CB_COLOR0_INFO[12:11].
         * Public Mesa ac_translate_colorswap + gfx10.json; do not change the
         * read-only shared builder or compensate in application shaders. */
        if (view->format == VK_FORMAT_B8G8R8A8_UNORM)
            result.registers[2].value=(result.registers[2].value & ~UINT32_C(0x1800)) | UINT32_C(0x0800);
        /* CB_COLOR0_INFO.NUMBER_TYPE (bits [10:8]) names how the hardware
         * interprets the 8_8_8_8 lanes the builder selected: UNORM for the
         * normalized targets, UINT for the integer one. Pinned gfx103 table:
         * NUMBER_UNORM = 0, NUMBER_UINT = 4; tests/test_color_attachment_
         * offsets.py recomputes the field and the values from that table. */
        if (ps5vk_color_target_format_is_integer(view->format))
            result.registers[2].value=(result.registers[2].value & ~UINT32_C(0x700)) |
                                      (UINT32_C(4) << 8u);
        /* The target's sample geometry, in the word the shared builder clears
         * (DXVK262-T06). */
        rc = color_target_samples(result.registers, image->info.samples);
        if (rc != VK_SUCCESS) return rc;
        result.count = PS5_COLOR_REGISTER_COUNT;
    } else {
        /* Only the explicitly configured BGRA attachment profile is enabled. */
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    *out = result; return VK_SUCCESS;
}

/* One array layer's footprint: the image path's own storage arithmetic, so the
 * layer a target names and the storage the image was given cannot disagree. */
VkResult ps5vk_native_layer_footprint(VkDevice d, VkImage image, VkDeviceSize *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = 0;
    if (!d || !image) return VK_ERROR_UNKNOWN;
    VkDeviceSize stride = 0, alignment = 0, bytes = 0;
    /* The footprint scales with the surface's own sample count, so layer N of a
     * multisampled array surface lands where the image's storage really put it
     * (the same call ps5vk_native_image_requirements sizes the image with). */
    VkResult rc = ps5vk_native_layered_storage_samples(image->info.format,
        image->info.extent.width, image->info.extent.height, 1u, image->info.samples,
        &stride, &alignment, &bytes);
    if (rc != VK_SUCCESS) return rc;
    *out = stride;
    return VK_SUCCESS;
}

VkResult ps5vk_native_layer_target(VkDevice d, VkImageView view, uint32_t layer,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT],
    struct ps5vk_target_registers *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (!d || !view || view->device != d || view->range.baseMipLevel ||
        view->range.levelCount != 1 || !view->range.layerCount)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkImage image = view->image;
    const uint64_t target_layer = (uint64_t)view->range.baseArrayLayer + layer;
    if (!image || target_layer >= image->info.arrayLayers)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    void *address; VkDeviceSize bytes;
    VkResult rc = ps5vk_image_span(d, image, &address, &bytes);
    if (rc != VK_SUCCESS) return rc;
    VkDeviceSize footprint = 0;
    rc = ps5vk_native_layer_footprint(d, image, &footprint);
    if (rc != VK_SUCCESS) return rc;
    /* The offset must be representable and the whole layer must fit, otherwise
     * a layer-addressed target cannot be honest for this surface. */
    if (!footprint || target_layer > (UINT64_MAX - (uintptr_t)address) / footprint)
        return VK_ERROR_UNKNOWN;
    const uint64_t offset = target_layer * footprint;
    if (offset > bytes || footprint > bytes - offset) return VK_ERROR_UNKNOWN;
    uintptr_t base = (uintptr_t)address + (uintptr_t)offset;
    if (!base || base >= (UINT64_C(1) << 48) || footprint > (UINT64_C(1) << 48) - base)
        return VK_ERROR_UNKNOWN;
    struct ps5vk_target_registers result = {0};
    const uint32_t width = image->info.extent.width, height = image->info.extent.height;
    if (view->format == VK_FORMAT_D32_SFLOAT || view->format == VK_FORMAT_D16_UNORM) {
        if (!(image->info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return VK_ERROR_UNKNOWN;
        /* No multisampled depth target exists on this path yet. */
        if (image->info.samples != VK_SAMPLE_COUNT_1_BIT) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (ps5_depth_build_d32_no_htile(result.registers, base, width, height)) return VK_ERROR_UNKNOWN;
        if (view->format == VK_FORMAT_D16_UNORM)
            result.registers[20].value = (result.registers[20].value & ~UINT32_C(3)) | UINT32_C(1);
        result.count = PS5_DEPTH_REGISTER_COUNT;
    } else if (view->format == VK_FORMAT_B8G8R8A8_UNORM ||
               view->format == VK_FORMAT_R8G8B8A8_UNORM ||
               ps5vk_color_target_integer_served(view->format)) {
        if (!(image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return VK_ERROR_UNKNOWN;
        if (ps5_color_build_target(result.registers, color_defaults, base, width, height))
            return VK_ERROR_UNKNOWN;
        if (view->format == VK_FORMAT_B8G8R8A8_UNORM)
            result.registers[2].value=(result.registers[2].value & ~UINT32_C(0x1800)) | UINT32_C(0x0800);
        if (ps5vk_color_target_format_is_integer(view->format))
            result.registers[2].value=(result.registers[2].value & ~UINT32_C(0x700)) |
                                      (UINT32_C(4) << 8u);
        rc = color_target_samples(result.registers, image->info.samples);
        if (rc != VK_SUCCESS) return rc;
        result.count = PS5_COLOR_REGISTER_COUNT;
    } else {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    *out = result; return VK_SUCCESS;
}

VkResult ps5vk_native_view_expand(uint32_t view_mask, uint32_t *view_indices,
    uint32_t capacity, uint32_t *view_count)
{
    if (!view_indices || !view_count || !capacity) return VK_ERROR_UNKNOWN;
    uint32_t ordered[PS5VK_MAX_VIEW_MASK_VIEWS];
    uint32_t count = 0;
    if (!view_mask) {
        /* Multiview disabled: exactly one view, index zero. */
        ordered[count++] = 0u;
    } else {
        /* Ascending by bit, not by whatever order the mask's bits are found in:
         * a view's layer and its ViewIndex have to agree for every view. */
        for (uint32_t bit = 0; bit < PS5VK_MAX_VIEW_MASK_VIEWS; ++bit) {
            if (!(view_mask & (UINT32_C(1) << bit))) continue;
            if (count == PS5VK_MAX_VIEW_MASK_VIEWS) return VK_ERROR_FEATURE_NOT_PRESENT;
            ordered[count++] = bit;
        }
    }
    if (count > capacity) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(view_indices, ordered, (size_t)count * sizeof(*view_indices));
    *view_count = count;
    return VK_SUCCESS;
}

VkResult ps5vk_native_view_layer_target(VkDevice d, VkImageView view, uint32_t view_index,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT],
    struct ps5vk_target_registers *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    /* The view carries this view's layer or it does not: a mask bit the
     * attachment view's own range does not cover has no legal target. The
     * backing check that follows is the shared per-layer one. */
    if (!view || view_index >= view->range.layerCount) {
        memset(out, 0, sizeof(*out));
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return ps5vk_native_layer_target(d, view, view_index, color_defaults, out);
}

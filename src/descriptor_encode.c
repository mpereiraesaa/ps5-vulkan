#include "descriptor_encode.h"
#include "texture_format.h"
#include "texture_layout.h"
#include "vk_image.h"
#include <string.h>

/* The compute UAV uses the same GFX10 T# fields as an ordinary 2D texture,
 * but carries no sampler. The exact supported image is one R32_UINT plane
 * over padded rows, with GENERAL layout and 256-byte address alignment. */
static VkResult storage_image_descriptor(VkDevice device,
    const VkDescriptorImageInfo *info, uint32_t out[8])
{
    if (!info || !info->imageView || info->imageLayout != VK_IMAGE_LAYOUT_GENERAL)
        return VK_ERROR_UNKNOWN;
    VkImageView view = info->imageView;
    VkImage image = view->image;
    if (view->device != device || !ps5vk_storage_image(image) ||
        image->layout != VK_IMAGE_LAYOUT_GENERAL ||
        view->format != VK_FORMAT_R32_UINT || view->view_type != VK_IMAGE_VIEW_TYPE_2D ||
        view->range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
        view->range.baseMipLevel || view->range.levelCount != 1 ||
        view->range.baseArrayLayer || view->range.layerCount != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct ps5vk_texture_format *format =
        ps5vk_texture_format_lookup(VK_FORMAT_R32_UINT);
    if (!format || !(format->witnessed & PS5VK_FORMAT_CAP_STORAGE_IMAGE))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t pitch; uint64_t needed;
    if (ps5vk_texture_row_layout(4, image->info.extent.width,
            image->info.extent.height, &pitch, &needed))
        return VK_ERROR_UNKNOWN;
    void *address = NULL; VkDeviceSize bytes = 0;
    if (ps5vk_image_span(device, image, &address, &bytes) != VK_SUCCESS ||
        !address || bytes < needed || ((uintptr_t)address & 255u) ||
        (uintptr_t)address >= (UINT64_C(1) << 48))
        return VK_ERROR_UNKNOWN;
    const uint64_t gpu = (uintptr_t)address;
    const uint32_t width = image->info.extent.width - 1;
    uint32_t words[8] = {0};
    words[0] = (uint32_t)(gpu >> 8);
    words[1] = (uint32_t)(gpu >> 40) | format->descriptor_format_word |
        ((width & 3u) << 30);
    words[2] = (width >> 2) | ((image->info.extent.height - 1) << 14) | (1u << 31);
    words[3] = ps5vk_texture_format_dst_sel(format) | (9u << 28);
    words[4] = pitch / 4u - 1u;
    words[5] = 4u << 20;
    memcpy(out, words, sizeof(words));
    return VK_SUCCESS;
}

VkResult ps5vk_buffer_descriptor(VkDevice device, const VkDescriptorBufferInfo *info,
                                 VkDeviceSize dynamic_offset, uint32_t out[4])
{
    if (!device || !info || !info->buffer || !out ||
        dynamic_offset > UINT64_MAX - info->offset)
        return VK_ERROR_UNKNOWN;
    void *address = NULL;
    VkDeviceSize bytes = 0;
    VkResult result = ps5vk_buffer_span(device, info->buffer,
                                        info->offset + dynamic_offset, info->range,
                                        &address, &bytes);
    if (result != VK_SUCCESS) return result;
    uint64_t gpu = (uintptr_t)address;
    if (!bytes || bytes > UINT32_MAX || gpu >= (UINT64_C(1) << 48) ||
        bytes > (UINT64_C(1) << 48) - gpu)
        return VK_ERROR_UNKNOWN;
    out[0] = (uint32_t)gpu;
    out[1] = (uint32_t)(gpu >> 32);
    out[2] = (uint32_t)bytes;
    /* The audited byte-addressed raw descriptor: stride zero, byte extent. A
     * typed/structured form with NUM_RECORDS in elements is not equivalent for
     * these byte-granular loads. */
    out[3] = 0x31016fac;
    return VK_SUCCESS;
}

VkResult ps5vk_descriptor_encode(VkDevice device,
    const struct ps5vk_compiled_program *program, uint32_t set_index, VkDescriptorSet set,
    const VkDeviceSize dynamic_offsets[PS5VK_MAX_DESCRIPTORS],
    uint32_t *table, size_t capacity_dwords)
{
    if (!device || !program || !set || !set->pool || set->pool->device != device ||
        !table || program->gfx != 1013 || !program->descriptor_count ||
        set_index >= PS5VK_MAX_SETS || program->descriptor_count > PS5VK_MAX_DESCRIPTORS)
        return VK_ERROR_UNKNOWN;
    uint32_t scratch[128] = {0};
    size_t extent = 0;
    for (uint32_t i = 0; i < program->descriptor_count; ++i) {
        const struct ps5vk_program_descriptor *p = &program->descriptors[i];
        if (p->set != set_index) continue;
        const uint32_t record_dwords =
            p->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 8u : 4u;
        if (p->binding >= PS5VK_MAX_BINDINGS ||
            p->table_dword > 128u - record_dwords || p->table_dword % 4 ||
            capacity_dwords < p->table_dword + record_dwords)
            return VK_ERROR_UNKNOWN;
        for (uint32_t j = 0; j < i; ++j)
            if (program->descriptors[j].set == set_index &&
                ((program->descriptors[j].table_dword < p->table_dword + record_dwords &&
                  p->table_dword < program->descriptors[j].table_dword +
                    (program->descriptors[j].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 8u : 4u)) ||
                 (program->descriptors[j].binding == p->binding &&
                  program->descriptors[j].element == p->element)))
                return VK_ERROR_UNKNOWN;
        const struct ps5vk_binding *binding = &set->signature.binding[p->binding];
        uint32_t index = binding->first + p->element;
        if (binding->count <= p->element || index >= PS5VK_MAX_DESCRIPTORS ||
            !set->defined[index] || set->signature.type[p->binding] != p->type)
            return VK_ERROR_UNKNOWN;
        if (p->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const VkDescriptorImageInfo *info = &set->images[index];
            if (!set->image_resources[index] || !info->imageView ||
                set->image_resources[index] != info->imageView->image ||
                storage_image_descriptor(device, info,
                    scratch + p->table_dword) != VK_SUCCESS)
                return VK_ERROR_UNKNOWN;
            if (extent < p->table_dword + 8) extent = p->table_dword + 8;
            continue;
        }
        if (p->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            VkBufferView view = set->texel_views[index];
            if (!view || view->device != device || !view->buffer) return VK_ERROR_UNKNOWN;
            const struct ps5vk_texture_format *entry =
                ps5vk_texture_format_lookup(view->format);
            if (!entry ||
                !(entry->capabilities & PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER) ||
                !entry->bytes_per_texel || entry->bytes_per_texel > 16)
                return VK_ERROR_UNKNOWN;
            const uint32_t element = entry->bytes_per_texel;
            void *address = NULL; VkDeviceSize bytes = 0;
            VkResult result = ps5vk_buffer_span(device, view->buffer, view->offset,
                                                view->range, &address, &bytes);
            if (result != VK_SUCCESS || !bytes || bytes % element ||
                bytes / element > UINT32_MAX)
                return VK_ERROR_UNKNOWN;
            uint64_t gpu = (uintptr_t)address;
            uint32_t *out = scratch + p->table_dword;
            /* Match RADV's gfx10 texel-buffer descriptor contract: a
             * structured element the size of one texel, NUM_RECORDS in texels,
             * structured OOB selection and RESOURCE_LEVEL set. A
             * raw/stride-zero descriptor is valid for SSBO byte addressing but
             * makes typed texel loads return the OOB value on this path. The
             * element format and the component completion both come from the
             * capability row, so no promoted format can be encoded with
             * another row's word or another row's channel order. */
            out[0] = (uint32_t)gpu;
            out[1] = (uint32_t)(gpu >> 32) | (element << 16);
            out[2] = (uint32_t)(bytes / element);
            out[3] = UINT32_C(0x11000000) |
                (ps5vk_texture_format_gfx10_format(entry) << 12) |
                ps5vk_texture_format_dst_sel(entry);
            if (extent < p->table_dword + 4) extent = p->table_dword + 4;
            continue;
        }
        const VkDescriptorBufferInfo *info = &set->buffers[index];
        VkDeviceSize dynamic = ps5vk_dynamic_descriptor_type(p->type) ?
            dynamic_offsets[i] : 0;
        uint32_t *out = scratch + p->table_dword;
        VkResult result = ps5vk_buffer_descriptor(device, info, dynamic, out);
        if (result != VK_SUCCESS) return result;
        if (extent < p->table_dword + 4) extent = p->table_dword + 4;
    }
    if(!extent)return VK_ERROR_UNKNOWN;
    memcpy(table, scratch, extent * sizeof(*table));
    return VK_SUCCESS;
}

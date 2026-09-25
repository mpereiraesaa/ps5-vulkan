#ifndef PS5VK_TEXTURE_FORMAT_H
#define PS5VK_TEXTURE_FORMAT_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* The single authoritative per-format capability table.
 *
 * ps5-vulkan does not have a generic notion of "supported format". Every
 * Vulkan decision - the feature bits reported by
 * vkGetPhysicalDeviceFormatProperties, the usage combinations accepted by
 * vkGetPhysicalDeviceImageFormatProperties and image creation, the image-view
 * and descriptor encoding, the padded-linear layout arithmetic and the
 * buffer/image copy planner - is derived from the capability flags below.
 *
 * Two independent masks describe each format:
 *
 *   capabilities  the operation is implemented in this tree: the GFX1013
 *                 encoding (or the documented absence of one) exists, the
 *                 layout/copy arithmetic is bounded, and host tests cover the
 *                 contract. This is a statement about ps5-vulkan's code only.
 *   witnessed     the enablement subset used by public queries and resource
 *                 creation. Sampled-image/filter promotions require two
 *                 identical-artifact console runs recorded in VALIDATION.md.
 *                 Two inherited uniform-texel-buffer roles (R32_SINT/SFLOAT)
 *                 have host object/encoder tests only, as API.md states;
 *                 their presence in this historical mask is NOT native proof.
 *
 * A capability may therefore be implemented and tested while still being
 * reported as a blocker: host tests do not prove hardware behaviour, so an
 * unwitnessed capability stays disabled until the physical diagnostic named in
 * conformance_inventory/physical_format_validation.json passes. Promotions are
 * a data change (move the flag from the `pending` column to the `witnessed`
 * column of the same row), never a code change.
 */
enum ps5vk_format_capability {
    /* Transfer and sampled-image roles. */
    PS5VK_FORMAT_CAP_TRANSFER_DST = 1u << 0,
    PS5VK_FORMAT_CAP_TRANSFER_SRC = 1u << 1,
    PS5VK_FORMAT_CAP_SAMPLED_IMAGE = 1u << 2,
    PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR = 1u << 3,
    /* Render-target roles. */
    PS5VK_FORMAT_CAP_COLOR_ATTACHMENT = 1u << 4,
    PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_READBACK = 1u << 5,
    PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_BLEND = 1u << 6,
    PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT = 1u << 7,
    /* Buffer roles. */
    PS5VK_FORMAT_CAP_VERTEX_BUFFER = 1u << 8,
    PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER = 1u << 9,
    PS5VK_FORMAT_CAP_STORAGE_TEXEL_BUFFER = 1u << 10,
    /* Storage-image roles. */
    PS5VK_FORMAT_CAP_STORAGE_IMAGE = 1u << 11,
    PS5VK_FORMAT_CAP_STORAGE_IMAGE_ATOMIC = 1u << 12,
    /* Blit roles. */
    PS5VK_FORMAT_CAP_BLIT_SRC = 1u << 13,
    PS5VK_FORMAT_CAP_BLIT_DST = 1u << 14,
};

/* Physical evidence established on the console, never inferred from host
 * tests, from the enum name, or from another project's format list. */
enum ps5vk_format_evidence {
    PS5VK_FORMAT_EVIDENCE_PS5_WITNESS = 1u << 0,
};

/* Where the encoding in a row comes from (bitmask; a row can combine both). */
enum ps5vk_format_provenance {
    PS5VK_FORMAT_PROVENANCE_NONE = 0u,
    /* GFX1013 descriptor word/selectors/texel size adapted from the pinned
     * GPL ps5-opengl reference (see LICENSING.md). */
    PS5VK_FORMAT_PROVENANCE_GPL_REFERENCE = 1u << 0,
    /* Encoding derived from the pinned Khronos registry alone: the packed
     * component order of the Vulkan enum is byte-identical to a row above and
     * no other hardware field changes. */
    PS5VK_FORMAT_PROVENANCE_REGISTRY_PACKING = 1u << 1,
    /* Numeric GFX10 data-format code from the pinned register enumeration. */
    PS5VK_FORMAT_PROVENANCE_GFX10_FORMAT_ENUM = 1u << 2,
};

/* Numeric category of a vertex attribute, as the PSBC/native vertex mapping
 * expects it. Defined here so the capability table can gate the vertex role. */
enum ps5vk_vertex_numeric {
    PS5VK_VERTEX_NUMERIC_NONE = 0,
    PS5VK_VERTEX_NUMERIC_FLOAT,
    PS5VK_VERTEX_NUMERIC_SINT,
    PS5VK_VERTEX_NUMERIC_UINT,
};

struct ps5vk_texture_format {
    VkFormat format;
    uint32_t bytes_per_texel;
    uint32_t descriptor_format_word;
    uint8_t selectors[4];
    /* Implemented operations (host contract complete). */
    uint32_t capabilities;
    /* Enabled operations; see the two legacy evidence exceptions above. */
    uint32_t witnessed;
    /* Provenance of the format encoding, if any (bitmask). */
    uint8_t provenance;
    /* Compressed formats are stored and copied in fixed-size blocks. For
     * ordinary texel formats this is a 1x1 block of bytes_per_texel bytes. */
    uint8_t block_width, block_height;
    uint8_t bytes_per_block;
};

const struct ps5vk_texture_format *ps5vk_texture_format_lookup(VkFormat format);
/* Ordered access to the whole table, for audits and the reporting dump. */
unsigned ps5vk_texture_format_count(void);
const struct ps5vk_texture_format *ps5vk_texture_format_at(unsigned index);

/* The GFX10 DST_SEL completion word of a row: X at bits 0-2, Y at 3-5, Z at
 * 6-8 and W at 9-11. Sampled-image and buffer descriptors share this encoding,
 * so it is derived from the row's selectors instead of being written twice. */
uint32_t ps5vk_texture_format_dst_sel(const struct ps5vk_texture_format *format);

/* The 8-bit GFX10 DATA_FORMAT value of a row. It sits at bits 20-27 in a
 * sampled-image descriptor word and at bits 12-19 in a buffer descriptor. */
uint32_t ps5vk_texture_format_gfx10_format(const struct ps5vk_texture_format *format);

/* Implemented / witnessed capability queries. Unknown formats answer zero. */
uint32_t ps5vk_texture_format_capabilities(VkFormat format);
VkBool32 ps5vk_texture_format_has(VkFormat format, uint32_t capability);
VkBool32 ps5vk_texture_format_witnessed(VkFormat format, uint32_t capability);

/* The published sampled-image role: the format has a complete GFX1013 encoding
 * and an on-console witness. This is the predicate the Vulkan-visible paths
 * (descriptor encoding, sampled upload, transfer recording) use. */
VkBool32 ps5vk_texture_format_sampled_image(VkFormat format);

/* The internal arithmetic role: the padded-linear encoding is implemented,
 * whether or not it is witnessed yet. Layout and copy planning use it so the
 * promised bytes-per-texel/stride/overflow contract is host-testable for
 * formats that are still waiting for their physical diagnostic. */
VkBool32 ps5vk_texture_format_sampled_encoding(VkFormat format);
VkBool32 ps5vk_texture_format_block_compressed(VkFormat format);

/* The published VkFormatProperties for a format, derived from the witnessed
 * capabilities only. linearTilingFeatures is always zero: this profile has no
 * linear-tiling image role. */
void ps5vk_texture_format_properties(VkFormat format, VkFormatProperties *out);

/* The exact image-usage combinations this profile implements for a format,
 * derived from the witnessed capabilities. Anything else must be rejected
 * before a resource is created. */
VkBool32 ps5vk_texture_format_image_usage(VkFormat format, VkImageUsageFlags usage);

/* Mutable-format views. ps5vk_texture_format_mutable() says whether an image
 * of this format may be created with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
 * ps5vk_texture_format_view_compatible() says whether a view (or a
 * VkImageFormatListCreateInfo entry) of view_format over an image of
 * image_format is an implemented reinterpretation. Only RGBA8 UNORM <-> SRGB
 * is. The view's usage is checked separately against the view format's own
 * witnessed capabilities. */
VkBool32 ps5vk_texture_format_mutable(VkFormat format);
VkBool32 ps5vk_texture_format_view_compatible(VkFormat image_format, VkFormat view_format);

#endif

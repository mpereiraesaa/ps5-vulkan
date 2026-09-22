#ifndef PS5VK_INPUT_ATTACHMENT_GATE_H
#define PS5VK_INPUT_ATTACHMENT_GATE_H
#include "vk_descriptor.h"
#include "vk_framebuffer.h"
#include "vk_image.h"
#include "vk_render_pass.h"
#include "runtime_draw_abi.h"

/* The exact layer count the promoted input-attachment resource was measured
 * with. It is this gate's own constant on purpose: the multiview reporting
 * floor happens to agree today, and a future change to that floor must not
 * silently widen or narrow this execution profile. */
enum { PS5VK_INPUT_ATTACHMENT_LAYER_COUNT = 6 };

/* The bounded one-input subpass-read profile.
 *
 * A subpass may read exactly one attachment through exactly one fragment
 * binding, and only in the shape this driver has measured end to end: the
 * resource is the promoted attachment itself - RGBA8 2D, one mip, one sample,
 * six layers, depth one, no flags, optimal tiling and exactly the
 * colour/transfer-source/input-attachment/transfer-destination usage - read
 * through one layer-0 2D colour view on the same device; the descriptor is the
 * one-element resource-only image record (32 bytes, no sampler words); its
 * recorded view IS the framebuffer view of the subpass's own input reference at
 * index 0, and both the descriptor and that reference name a READ layout - the
 * two the render-pass frontend admits and this executor can leave the
 * attachment in: GENERAL, which the multiview witness declares, and
 * SHADER_READ_ONLY_OPTIMAL, which the pinned multisample oracle declares for the
 * colour attachment its fetch subpasses read. A forward dependency - BY_REGION
 * or not - still carries the colour write of subpass 0 to the fragment
 * input-attachment read of the later subpass, and the executor's own boundary
 * barrier orders the read whether or not the pass declares one. Every other
 * shape fails closed here, before any packet is emitted, and this is the only
 * place native execution learns what an input attachment may be.
 *
 * The function takes the real objects rather than a summary of them so the
 * same rule the driver executes is the rule the host tests exercise. It never
 * reads a sampler, and it never treats an input attachment as a combined
 * T#/S# pair. */
VkResult ps5vk_input_attachment_gate(VkDevice device, VkRenderPass pass, uint32_t subpass,
    VkFramebuffer framebuffer, VkDescriptorSet set, const struct ps5vk_binding *binding,
    uint32_t binding_index, VkDescriptorType type, unsigned element_index,
    uint32_t input_binding_count, const struct ps5vk_runtime_draw_abi *abi,
    unsigned set_index);

#endif

#ifndef PS5VK_COLOR_ATTACHMENT_CONTRACT_H
#define PS5VK_COLOR_ATTACHMENT_CONTRACT_H
#include <vulkan/vulkan_core.h>

/* Bounded per-attachment colour contract of the DXVK v2.6.2 profile.
 *
 * The native path programmes one CB_COLOR0 target, one CB_BLEND0 equation and
 * one CB_TARGET_MASK, so this profile serves exactly one colour attachment.
 * That bound is what keeps `independentBlend` a blocker: the feature's whole
 * obligation is that each attachment carries its own blend state, which a
 * single attachment cannot exercise, and no native witness could show it. The
 * bound is a compile-time constant so the refusal is identical in every build
 * and can be pinned by tests; a slice that can render a second target - render
 * pass, framebuffer, pipeline key, CB_COLOR1/CB_BLEND1 and a second fragment
 * export - is the one that raises it.
 *
 * `maxFragmentDualSrcAttachments` and the dual-source contract are separate:
 * dual source adds a second *source* to attachment zero, not a second target. */
enum { PS5VK_MAX_COLOR_ATTACHMENTS = 1 };

/* A pipeline that declares a count the profile cannot render, or an operation
 * the backend does not programme, is refused where the application can see it
 * rather than accepted and failed later. */
int ps5vk_color_attachment_count_supported(uint32_t count);
int ps5vk_color_blend_state_shape_supported(const VkPipelineColorBlendStateCreateInfo *state);

/* True when this attachment state consumes the fragment module's secondary
 * export. Such a state also needs dualSrcBlend enabled on the logical device
 * and the compiler-proven secondary export; the caller checks those. */
int ps5vk_color_attachment_uses_src1(const VkPipelineColorBlendAttachmentState *state);

#endif

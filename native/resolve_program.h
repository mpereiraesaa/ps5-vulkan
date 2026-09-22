#ifndef PS5VK_RESOLVE_PROGRAM_H
#define PS5VK_RESOLVE_PROGRAM_H
#include <vulkan/vulkan.h>

/* The driver's own resolve stages (DXVK262-T06).
 *
 * A pass whose subpass declares a resolve target promises that the resolved
 * result reaches that target, and this profile produces it with the only
 * mechanism it has measured: a draw that reads every sample of the
 * multisampled colour attachment through the resource-only record and writes
 * their average into the single-sample target. That draw needs a vertex stage
 * and an averaging fragment stage the DRIVER owns - not a probe's payload - so
 * one fragment per served sample count is compiled at build time
 * (tools/build_resolve_shaders.py) and paired here with the oversized-triangle
 * vertex stage the sample-rate witness uses.
 *
 * The pair is compiled through the same runtime compiler the pipeline objects
 * use, against the same descriptor contract: set 0 binding 0 is the input
 * attachment and nothing else, because an averaging stage reads exactly that.
 * Acquisition returns an owned program; the caller releases it when the pass
 * that needed it is done. */
struct ps5vk_resolve_program {
    const void *pair;
};

VkResult ps5vk_resolve_program_acquire(VkDevice device, VkSampleCountFlagBits samples,
    struct ps5vk_resolve_program *out);
void ps5vk_resolve_program_release(VkDevice device, struct ps5vk_resolve_program *program);

#endif

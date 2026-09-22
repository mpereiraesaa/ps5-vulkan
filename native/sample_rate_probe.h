#ifndef PS5VK_SAMPLE_RATE_PROBE_H
#define PS5VK_SAMPLE_RATE_PROBE_H
#include <vulkan/vulkan.h>

/* DXVK262-T06 multisampled colour target probe.
 *
 * One multisampled colour attachment is created, cleared through a render pass
 * the front end and the native queue both accept, submitted, and then read
 * back from its OWN storage - no resolve, no copy, no second target. The
 * verdict is the storage contract this profile claims for a multisampled
 * surface: a whole-surface clear leaves one distinct 32-bit value across a
 * span that is the sample count times the single-sample footprint, which a
 * surface sized for one sample could not satisfy. */
struct ps5vk_sample_rate_probe_params {
    VkSampleCountFlagBits samples;
    uint32_t extent;
    float clear[4];
};

VkResult ps5vk_sample_rate_probe(VkDevice, const struct ps5vk_sample_rate_probe_params *);
#endif

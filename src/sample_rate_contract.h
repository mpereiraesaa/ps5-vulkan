#ifndef PS5VK_SAMPLE_RATE_CONTRACT_H
#define PS5VK_SAMPLE_RATE_CONTRACT_H
#include <vulkan/vulkan_core.h>

/* The multisample counts this profile's native path is built for (DXVK262-T06).
 *
 * Vulkan 1.0 only requires one sample, so a device that claims more has to
 * rasterize, resolve and read back at those counts. 2x and 4x are the counts
 * this frontend and its CB_COLOR0 target are written for: the pinned gfx103
 * register table carries NUM_SAMPLES/NUM_FRAGMENTS in CB_COLOR0_ATTRIB, the
 * pinned compiler accepts 2 and 4 as rasterization_samples, and the applicable
 * focused CTS leaves exist for both. 8x and above are NOT claimed - nothing on
 * this path measured them, and the sample-count limits are reported from the
 * platform mask rather than from this envelope.
 *
 * This mask is the compile-time envelope only. What a live device reports and
 * accepts is ps5vk_platform_sample_counts(supported_features) in
 * src/vk_internal.h, which is exactly this mask when the platform carries
 * PS5VK_FEATURE_SAMPLE_RATE_SHADING and 1x otherwise, so no build can advertise
 * a count whose native path does not exist. */
static inline VkSampleCountFlags ps5vk_sample_count_mask(void)
{
    return VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
}

/* The number of samples a flag names, or 0 when this profile does not
 * implement that count. Every consumer that has to size storage, index sample
 * data or build a mask derives the count from here instead of repeating the
 * switch. */
static inline uint32_t ps5vk_sample_count_number(VkSampleCountFlagBits samples)
{
    switch (samples) {
    case VK_SAMPLE_COUNT_1_BIT: return 1u;
    case VK_SAMPLE_COUNT_2_BIT: return 2u;
    case VK_SAMPLE_COUNT_4_BIT: return 4u;
    default: return 0u;
    }
}

static inline int ps5vk_sample_count_implemented(VkSampleCountFlagBits samples)
{
    return ps5vk_sample_count_number(samples) != 0u;
}

/* CB_COLOR0_ATTRIB's NUM_SAMPLES and NUM_FRAGMENTS fields carry log2 of the
 * sample count, not the count: the pinned Mesa source writes
 * S_028C74_NUM_SAMPLES(util_logbase2(num_samples)) for a colour surface
 * (src/amd/common/ac_descriptors.c). The register's field positions are pinned
 * by tests/test_sample_count_registers.py against the same table. */
static inline uint32_t ps5vk_sample_count_log2(VkSampleCountFlagBits samples)
{
    switch (samples) {
    case VK_SAMPLE_COUNT_2_BIT: return 1u;
    case VK_SAMPLE_COUNT_4_BIT: return 2u;
    default: return 0u;
    }
}

/* The sample mask that covers every implemented sample of a count. A pipeline
 * whose pSampleMask asks for bits beyond this count is refused rather than
 * silently masked, because no register on this path carries it. */
static inline VkSampleMask ps5vk_sample_count_full_mask(VkSampleCountFlagBits samples)
{
    const uint32_t count = ps5vk_sample_count_number(samples);
    return count ? (VkSampleMask)((UINT32_C(1) << count) - 1u) : 0u;
}

#endif

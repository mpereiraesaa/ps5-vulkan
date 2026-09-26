#ifndef PS5VK_VK_CORE_VERSION_H
#define PS5VK_VK_CORE_VERSION_H

#include "vk_internal.h"

/* Core Vulkan 1.1-1.3 query/creation structures and core command names.
 *
 * Everything here is gated on the version the physical device reports, which
 * controls which structures are answered and which core aliases resolve. Values
 * are projections of the existing per-extension answers, never separate
 * claims. */

/* The version the physical device reports. */
uint32_t ps5vk_physical_api_version(VkPhysicalDevice p);
/* Device-level functionality follows the lower of the instance version and
 * the physical device version. */
uint32_t ps5vk_effective_api_version(VkPhysicalDevice p);

/* Fills a recognised core structure in a Features2/Properties2 chain and
 * returns 1; returns 0 for anything else, leaving it untouched. */
int ps5vk_core_version_features(VkPhysicalDevice p, VkBaseOutStructure *next);
int ps5vk_core_version_properties(VkPhysicalDevice p, VkBaseOutStructure *next);

/* vkCreateDevice: consumes a VkPhysicalDeviceVulkan1{1,2,3}Features structure.
 * Returns VK_ERROR_FEATURE_NOT_PRESENT for other structures, for structures
 * above the effective version and for any requested member the device does
 * not report; `seen` rejects duplicates. */
VkResult ps5vk_core_version_enable(VkPhysicalDevice p, const VkBaseInStructure *next,
                                   uint32_t *seen, uint32_t *enabled,
                                   uint32_t *enabled_t09);
VkResult ps5vk_core_version_validate_chain(const VkBaseInStructure *chain);

/* vkCreateDevice: behaviour promoted into the effective version is on without
 * the extension, so the per-extension gates (d->*_extension_enabled) open. */
void ps5vk_core_version_promote(VkDevice d);

#endif

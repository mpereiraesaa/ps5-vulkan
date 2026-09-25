/* DIAGNOSTIC compatibility translation layer; see compat_layer.cpp. Built
 * only into the diagnostic-compat variant (DXVK_NATIVE_COMPAT_LAYER=1). */
#ifndef DXVK_NATIVE_COMPAT_LAYER_H
#define DXVK_NATIVE_COMPAT_LAYER_H

#include <vulkan/vulkan.h>

#define DXVK_NATIVE_COMPAT_LAYER_VERSION 1

void compat_features2(PFN_vkGetPhysicalDeviceFeatures2 real, VkPhysicalDevice physical,
                      VkPhysicalDeviceFeatures2 *features);
void compat_properties2(PFN_vkGetPhysicalDeviceProperties2 real, VkPhysicalDevice physical,
                        VkPhysicalDeviceProperties2 *properties);
VkResult compat_create_device(PFN_vkCreateDevice real, VkPhysicalDevice physical,
                              const VkDeviceCreateInfo *info,
                              const VkAllocationCallbacks *allocator, VkDevice *device);
/* Adds VK_KHR_get_physical_device_properties2; returns nullptr if unchanged. */
const char *compat_add_instance_extension(const VkInstanceCreateInfo *info,
                                          VkInstanceCreateInfo *patched,
                                          const char **storage, uint32_t capacity);

#endif

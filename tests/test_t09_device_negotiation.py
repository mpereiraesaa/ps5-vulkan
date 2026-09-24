"""Compile the public device entry points against the host platform fixture."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SOURCE = r'''
#define main unused_device_test_main
#include "tests/test_vk_device.c"
#undef main
int main(void)
{
    VkInstance instance_handle = features2_instance();
    VkPhysicalDevice p = physical(instance_handle);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue;
    VkDeviceCreateInfo info = device_info(&queue, &priority);
    VkPhysicalDeviceImagelessFramebufferFeatures feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES,
        .imagelessFramebuffer = VK_TRUE};
    VkPhysicalDeviceFeatures2 chain = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feature};
    info.pNext = &chain;
    VkDevice device = VK_NULL_HANDLE;
    feature.imagelessFramebuffer = VK_FALSE;
    vkGetPhysicalDeviceFeatures2KHR(p, &chain);
    assert(feature.imagelessFramebuffer == VK_FALSE);
    feature.imagelessFramebuffer = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT);
    p->platform.supported_features_t09 = PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER;
    feature.imagelessFramebuffer = VK_FALSE;
    vkGetPhysicalDeviceFeatures2KHR(p, &chain);
    assert(feature.imagelessFramebuffer == VK_TRUE);
    feature.imagelessFramebuffer = 2;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN);
    feature.imagelessFramebuffer = VK_TRUE;
    VkPhysicalDeviceImagelessFramebufferFeatures duplicate = feature;
    feature.pNext = &duplicate;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN);
    feature.pNext = NULL;
    const char *extension = VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &extension;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT);
    info.enabledExtensionCount = 0;
    info.ppEnabledExtensionNames = NULL;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(device->enabled_features_t09 & PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER);
    vkDestroyDevice(device, NULL);
    feature.imagelessFramebuffer = VK_FALSE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(!(device->enabled_features_t09 & PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER));
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance_handle, NULL);
    return 0;
}
'''


class T09DeviceNegotiation(unittest.TestCase):
    def test_imageless_pnext_and_public_extension_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "negotiation.c"
            binary = Path(directory) / "negotiation"
            source.write_text(SOURCE)
            command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-error=return-type",
                       "-ffunction-sections", "-fdata-sections",
                       "-I" + str(ROOT), "-I" + str(ROOT / "src"),
                       "-I" + str(ROOT / "third_party/vulkan-headers/include"),
                       str(source),
                       *[str(ROOT / "src" / name) for name in (
                           "vk_device.c", "vk_alloc.c", "vk_memory.c",
                           "texture_format.c", "texture_layout.c",
                           "compilation_cache.c")],
                       "-Wl,--gc-sections", "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()

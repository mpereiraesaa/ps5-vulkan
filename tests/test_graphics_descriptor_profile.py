from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GraphicsDescriptorProfileTests(unittest.TestCase):
    def test_buffer_routing_and_unknown_types(self):
        source = r'''
#include "native/graphics_descriptor_profile.h"
#include <assert.h>
int main(void) {
    VkDescriptorType buffers[]={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC};
    for(unsigned i=0;i<4;++i) {
        assert(ps5vk_graphics_buffer_type(buffers[i]));
        assert(ps5vk_graphics_descriptor_type(buffers[i]));
    }
    assert(!ps5vk_graphics_buffer_type(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT));
    assert(ps5vk_graphics_descriptor_type(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT));
    assert(!ps5vk_graphics_descriptor_type(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
    assert(!ps5vk_graphics_descriptor_type((VkDescriptorType)-1));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "profile"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT), "-I", str(ROOT / "third_party/vulkan-headers/include"),
                            "-x", "c", "-", "-o", str(binary)],
                           input=source, text=True, capture_output=True, check=True)
            subprocess.run([str(binary)], check=True)

"""The device-extension enumeration array must hold every conditional push."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DeviceExtensionSlots(unittest.TestCase):
    def test_declared_push_count_matches_source_and_fits(self):
        text = (ROOT / "src/vk_device.c").read_text()
        start = text.index("VKAPI_CALL vkEnumerateDeviceExtensionProperties")
        end = text.index("VKAPI_CALL vkEnumerateDeviceLayerProperties", start)
        body = text[start:end]
        pushes = len(re.findall(r"properties\[total\+\+\]", body))
        declared = re.search(r"DEVICE_EXTENSION_PUSHES = (\d+), DEVICE_EXTENSION_SLOTS = (\d+)",
                             body)
        self.assertIsNotNone(declared)
        self.assertEqual(pushes, int(declared.group(1)))
        self.assertLessEqual(pushes, int(declared.group(2)))
        self.assertIn("properties[DEVICE_EXTENSION_SLOTS]", body)


if __name__ == "__main__":
    unittest.main()

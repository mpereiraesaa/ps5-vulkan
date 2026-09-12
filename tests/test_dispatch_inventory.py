"""Catch implemented entry points omitted from the static-library lookup table.

This audits source inventory, not function behavior or Vulkan conformance.
Compiled pointer/scope assertions live in test_vk_device.c.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DispatchInventory(unittest.TestCase):
    def test_every_implemented_entry_is_registered_once(self):
        definitions = []
        for source in (ROOT / "src").glob("vk_*.c"):
            definitions.extend(re.findall(
                r"VKAPI_ATTR\s+\w+\s+VKAPI_CALL\s+(vk\w+)\s*\(",
                source.read_text()))
        entries = re.findall(r"ENTRY\((vk\w+),\s*(?:GLOBAL|INSTANCE|DEVICE)\)",
                             (ROOT / "src/vk_dispatch.c").read_text())
        self.assertTrue(definitions)
        self.assertEqual(len(definitions), len(set(definitions)))
        self.assertEqual(len(entries), len(set(entries)))
        self.assertEqual(set(definitions), set(entries))

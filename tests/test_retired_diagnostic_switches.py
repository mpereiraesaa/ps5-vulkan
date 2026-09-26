"""Promoted routes keep no measurement switch.

Each switch below selected a route that native evidence has since promoted into
the shipping profile. The switch must not return in any tracked source; the
dated history in VALIDATION.md and API.md is the only place that may name it.
"""

import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HISTORY = {"VALIDATION.md", "API.md", Path(__file__).relative_to(ROOT).as_posix()}
RETIRED = tuple("PS5VK_" + name + "_DIAGNOSTIC" for name in (
    "SHADER_DEMOTE",
    "SYNCHRONIZATION2",
    "DXVK_FORMAT_ROUTES",
    "STORAGE_TEXEL",
    "IMAGELESS_FRAMEBUFFER",
    "ROBUSTNESS2",
    "DXVK_ROUTES",
    "DESCRIPTOR_UPDATE_TEMPLATE",
    "DXVK_RENDER",
))


class RetiredDiagnosticSwitches(unittest.TestCase):
    def test_no_tracked_source_names_a_retired_switch(self):
        for switch in RETIRED:
            found = subprocess.run(["git", "grep", "-l", switch], cwd=ROOT, text=True,
                                   capture_output=True).stdout.split()
            with self.subTest(switch=switch):
                self.assertEqual([path for path in found if path not in HISTORY], [])


if __name__ == "__main__":
    unittest.main()

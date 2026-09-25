"""The default-off PS5VK_DXVK_ROUTES_DIAGNOSTIC measurement switch: one guarded
platform block that reports exactly the three memory routes, plumbed through
the SDK build and excluded from the shipping capability probe."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
NAME = "PS5VK_DXVK_ROUTES_DIAGNOSTIC"


class DxvkRoutesDiagnostic(unittest.TestCase):
    def test_platform_block_reports_only_the_memory_routes(self):
        source = (ROOT / "native/platform_ps5.c").read_text()
        guard = f"#if defined({NAME}) && {NAME}"
        self.assertEqual(source.count(guard), 1)
        block = source[source.index(guard):]
        block = block[:block.index("#endif")]
        bits = set(re.findall(r"PS5VK_T09_FEATURE_[A-Z0-9_]+", block))
        self.assertEqual(bits, {"PS5VK_T09_FEATURE_GET_MEMORY_REQUIREMENTS2",
                                "PS5VK_T09_FEATURE_DEDICATED_ALLOCATION",
                                "PS5VK_T09_FEATURE_BIND_MEMORY2"})

    def test_switch_is_plumbed_and_excluded_from_the_shipping_probe(self):
        build = (ROOT / "tools/build_sdk.py").read_text()
        self.assertIn(f'"{NAME}"', build)
        profile = (ROOT / "tools/check_dxvk_profile.py").read_text()
        self.assertIn(f'"{NAME}"', profile)


if __name__ == "__main__":
    unittest.main()

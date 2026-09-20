import unittest
from tools.build_sdk import tess_ring_flags


class TessSDKProfileTests(unittest.TestCase):
    def test_default_diagnostic_flags_are_off(self):
        self.assertEqual(tess_ring_flags({}), [])
        self.assertEqual(tess_ring_flags({"PS5VK_TESS_RING_QUERY": "0"}), [])

    def test_extended_records_are_explicit(self):
        self.assertEqual(tess_ring_flags({"PS5VK_TESS_RING_QUERY": "4"}),
                         ["-DPS5VK_TESS_RING_QUERY=4"])

    def test_partial_diagnostic_modes_and_unknown_values_rejected(self):
        for mode in ("", "1", "2", "3", "5", "true", "4 -DOTHER=1"):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                tess_ring_flags({"PS5VK_TESS_RING_QUERY": mode})

    def test_public_api_experiment_forbids_bypass(self):
        env = {"PS5VK_TESS_EXPERIMENTAL_API": "1", "PS5VK_TESS_RING_QUERY": "4",
               "PS5VK_TESS_OFFCHIP_BIND": "1"}
        self.assertEqual(tess_ring_flags(env), ["-DPS5VK_TESS_RING_QUERY=4",
                         "-DPS5VK_TESS_EXPERIMENTAL_API=1"])
        self.assertEqual(tess_ring_flags({"PS5VK_TESS_EXPERIMENTAL_API": "1"}),
                         ["-DPS5VK_TESS_EXPERIMENTAL_API=1"])
        for name in ("PS5VK_OPTIONAL_STAGE_DIAGNOSTIC", "PS5VK_TESS_PROBE"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                tess_ring_flags({**env, name: "1"})
        with self.assertRaises(ValueError):
            tess_ring_flags({**env, "PS5VK_TESS_EXPERIMENTAL_API": "yes"})

    def test_bypass_is_forbidden_without_diagnostic_switches_too(self):
        for name in ("PS5VK_OPTIONAL_STAGE_DIAGNOSTIC", "PS5VK_TESS_PROBE"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "bypass"):
                tess_ring_flags({"PS5VK_TESS_EXPERIMENTAL_API":"1",name:"1"})


if __name__ == "__main__":
    unittest.main()

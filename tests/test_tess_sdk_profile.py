import unittest
from tools.build_sdk import tess_ring_flags


class TessSDKProfileTests(unittest.TestCase):
    def test_default_is_off(self):
        self.assertEqual(tess_ring_flags({}), [])
        self.assertEqual(tess_ring_flags({"PS5VK_TESS_RING_QUERY": "0"}), [])

    def test_complete_queue_lifecycle_is_explicit(self):
        self.assertEqual(tess_ring_flags({"PS5VK_TESS_RING_QUERY": "4"}),
                         ["-DPS5VK_TESS_RING_QUERY=4"])

    def test_partial_diagnostic_modes_and_unknown_values_rejected(self):
        for mode in ("", "1", "2", "3", "5", "true", "4 -DOTHER=1"):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                tess_ring_flags({"PS5VK_TESS_RING_QUERY": mode})

    def test_public_api_experiment_requires_ring_and_forbids_bypass(self):
        env = {"PS5VK_TESS_EXPERIMENTAL_API": "1", "PS5VK_TESS_RING_QUERY": "4"}
        self.assertEqual(tess_ring_flags(env), ["-DPS5VK_TESS_RING_QUERY=4",
                         "-DPS5VK_TESS_EXPERIMENTAL_API=1"])
        with self.assertRaises(ValueError):
            tess_ring_flags({"PS5VK_TESS_EXPERIMENTAL_API": "1"})
        for name in ("PS5VK_OPTIONAL_STAGE_DIAGNOSTIC", "PS5VK_TESS_PROBE"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                tess_ring_flags({**env, name: "1"})
        with self.assertRaises(ValueError):
            tess_ring_flags({**env, "PS5VK_TESS_EXPERIMENTAL_API": "yes"})


if __name__ == "__main__":
    unittest.main()

"""Reject conflicting lifecycle experiments before compilation or deployment."""
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeDiagnosticOptions(unittest.TestCase):
    def test_runtime_graphics_recipe_uses_system_close(self):
        result = subprocess.run(
            ["make", "-n", "native-runtime-graphics", "GRAPHICS_CONTROL=fixture",
             "GLSLANG=glslang-test"], cwd=ROOT, capture_output=True, text=True,
            check=True)
        self.assertIn("PS5VK_RUNTIME_GRAPHICS=1", result.stdout)
        self.assertIn("PS5VK_SHELL_CLOSE=1", result.stdout)
        self.assertIn("PS5VK_GLSLANG=glslang-test", result.stdout)
        self.assertIn("PS5VK_GRAPHICS_DRAW=1", result.stdout)
        self.assertIn("PS5VK_GRAPHICS_PRESENT=1", result.stdout)

    def test_witnesses_require_graphics_api(self):
        self.rejected({"PS5VK_GRAPHICS_WITNESSES":"1"},"requires graphics profile API")

    def test_witnesses_reject_unknown_mode(self):
        self.rejected({"PS5VK_GRAPHICS_WITNESSES":"3"},"must be 0 or 1")

    def test_continuous_requires_graphics_api(self):
        self.rejected({"PS5VK_GRAPHICS_CONTINUOUS": "1"}, "requires graphics profile API")

    def test_continuous_rejects_unknown_mode(self):
        self.rejected({"PS5VK_GRAPHICS_CONTINUOUS": "2"}, "must be 0 or 1")

    def test_continuous_cannot_be_deliberately_slowed(self):
        self.rejected({"PS5VK_GRAPHICS_CONTINUOUS": "1", "PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_GRAPHICS_OBSERVE": "1"}, "cannot use observation pauses")

    def test_observation_requires_graphics_api(self):
        self.rejected({"PS5VK_GRAPHICS_OBSERVE": "1"}, "requires graphics profile API")

    def test_observation_rejects_unknown_mode(self):
        self.rejected({"PS5VK_GRAPHICS_OBSERVE": "2"}, "must be 0 or 1")

    def rejected(self, options, message):
        env = {k: v for k, v in os.environ.items() if not k.startswith("PS5VK_")}
        env.update(options)
        result = subprocess.run([sys.executable, "tools/build_native.py"], cwd=ROOT,
                                env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn(message, result.stderr)

    def test_shell_close_requires_graphics_api(self):
        self.rejected({"PS5VK_SHELL_CLOSE": "1"}, "requires graphics profile API")

    def test_exit_control_requires_graphics_api(self):
        self.rejected({"PS5VK_EXIT_CONTROL": "3"}, "requires the graphics profile API")

    def test_unknown_exit_control(self):
        self.rejected({"PS5VK_EXIT_CONTROL": "4"}, "must be 0")

    def test_cannot_claim_shell_close_with_retained_module(self):
        self.rejected({"PS5VK_SHELL_CLOSE": "1", "PS5VK_KEEP_AGC_MODULE": "1",
                       "PS5VK_GRAPHICS_API": "unused"}, "Do not combine")

    def test_cannot_mix_exit_probe_with_shell_close(self):
        self.rejected({"PS5VK_SHELL_CLOSE": "1", "PS5VK_EXIT_CONTROL": "1",
                       "PS5VK_GRAPHICS_API": "unused"}, "Do not combine")

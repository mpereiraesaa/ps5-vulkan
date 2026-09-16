"""Reject conflicting lifecycle experiments before compilation or deployment."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeDiagnosticOptions(unittest.TestCase):
    def test_binding_diagnostic_uses_tested_workload_capacity_gate(self):
        source = (ROOT / "native/graphics_main.c").read_text()
        self.assertIn("ps5vk_graphics_vertex_bindings_available(&device_props.limits,", source)
        self.assertNotIn("device_props.limits.maxVertexInputBindings!=1", source)
        self.assertIn("PS5VK_GRAPHICS_SCISSOR_PROBE==13?16u:1u", source)

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

    def test_core_sampler_probe_is_a_scene_diagnostic(self):
        self.rejected({"PS5VK_GRAPHICS_SCISSOR_PROBE": "6"},
                      "requires graphics profile API")

    def test_sampled_format_probe_is_a_scene_diagnostic(self):
        self.rejected({"PS5VK_GRAPHICS_SCISSOR_PROBE": "7"},
                      "requires graphics profile API")
        source = (ROOT / "native/graphics_main.c").read_text()
        self.assertIn("PS5VK_GRAPHICS_SCISSOR_PROBE!=7 && PS5VK_GRAPHICS_SCISSOR_PROBE!=9",
                      source)

    def test_sampled_filter_probe_has_exact_oracle(self):
        self.rejected({"PS5VK_GRAPHICS_SCISSOR_PROBE": "9"},
                      "requires graphics profile API")
        source = (ROOT / "native/graphics_main.c").read_text()
        self.assertIn("PS5VK_SAMPLED_FORMAT_CASES*PS5VK_SAMPLED_FORMAT_FILTER_TRIALS",
                      source)
        self.assertIn("PS5VK_SAMPLED_FILTER_READBACK", source)
        self.assertIn("valid=expected==373248u && !other", source)

    def test_sampled_filter_probe_excludes_generic_scene_oracle(self):
        self.rejected({"PS5VK_GRAPHICS_SCISSOR_PROBE": "9"},
                      "requires graphics profile API")
        source = (ROOT / "native/graphics_main.c").read_text()
        self.assertIn("PS5VK_GRAPHICS_SCISSOR_PROBE!=9 &&", source)
        self.assertIn("PS5VK_SAMPLED_FORMAT_CASES*PS5VK_SAMPLED_FORMAT_FILTER_TRIALS",
                      source)

    def test_integer_sampled_probe_is_a_scene_diagnostic(self):
        self.rejected({"PS5VK_GRAPHICS_SCISSOR_PROBE": "10"},
                      "requires graphics profile API")
        source = (ROOT / "native/graphics_main.c").read_text()
        self.assertIn("PS5VK_INTEGER_SAMPLED_CASES_PER_SIGN", source)
        self.assertIn("PS5VK_INTEGER_SAMPLED_READBACK", source)
        self.assertIn("PS5VK_GRAPHICS_SCISSOR_PROBE!=10", source)

    def test_mip_lod_bias_is_bounded_and_mipmap_only(self):
        self.rejected({"PS5VK_MIP_LOD_BIAS": "1"}, "must be -2, 0, or 2")
        self.rejected({"PS5VK_MIP_LOD_BIAS": "2"}, "only for mipmap diagnostic")

    def test_mip_lod_bias_cannot_hide_behind_forced_clamp(self):
        self.rejected({"PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_GRAPHICS_SCISSOR_PROBE": "12",
                       "PS5VK_MIP_LOD_BIAS": "2", "PS5VK_MIP_FORCE_LOD": "1"},
                      "cannot be combined")

    def test_vertex_format_probe_is_nonindexed_runtime_draw(self):
        source = (ROOT / "native/graphics_main.c").read_text()
        self.assertIn("index_buffer && PS5VK_GRAPHICS_SCISSOR_PROBE!=8", source)
        # Deliberately exercise a legal byte-granular Vulkan offset which the
        # native GFX1013 SRD cannot encode directly.  Draw preparation must
        # preserve 25 rather than silently rounding it down to 24.
        self.assertIn("const size_t start=25u;", source)
        self.assertIn("binding_offset=%zu", source)

    def test_stale_graphics_library_signature_is_rejected(self):
        parent = ROOT / "build/graphics"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="stale-contract-", dir=parent) as folder:
            Path(folder, "manifest.json").write_text(json.dumps({
                "scope": "host-graphics-control-only",
                "native_input_version": 2,
            }))
            env = {k: v for k, v in os.environ.items() if not k.startswith("PS5VK_")}
            env["PS5VK_GRAPHICS_API"] = folder
            result = subprocess.run([sys.executable, "tools/build_native.py"], cwd=ROOT,
                                    env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("lacks current interpolation ABI", result.stderr)

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

    def test_multiview_diagnostic_is_graphics_only(self):
        self.rejected({"PS5VK_MULTIVIEW_DIAGNOSTIC": "1"},
                      "requires the graphics profile API")
        self.rejected({"PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_MULTIVIEW_DIAGNOSTIC": "2"},
                      "must be 0 or 1")

    def test_multiview_instance_probe_needs_the_view_witness(self):
        """The instance witness is the six-view scene with one instance at the
        pinned first instance, so it cannot be selected on its own."""
        self.rejected({"PS5VK_MULTIVIEW_INSTANCE_PROBE": "1"},
                      "requires PS5VK_MULTIVIEW_VIEW_PROBE=1")
        self.rejected({"PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_RUNTIME_GRAPHICS": "1",
                       "PS5VK_MULTIVIEW_VIEW_PROBE": "1",
                       "PS5VK_MULTIVIEW_DIAGNOSTIC": "1",
                       "PS5VK_MULTIVIEW_INSTANCE_PROBE": "2"},
                      "must be 0 or 1")

    def test_multiview_instance_witness_stays_private_and_pinned(self):
        """The switch is private, and the artifact manifest pins the value this
        slice is about together with the shader that carries it."""
        for public in (ROOT / "include").rglob("*.h"):
            text = public.read_text()
            self.assertNotIn("PS5VK_MULTIVIEW_INSTANCE_PROBE", text,
                             f"{public} must not expose the instance switch")
            self.assertNotIn("maxMultiviewInstanceIndex", text,
                             f"{public} must not report the property")
        build = (ROOT / "tools/build_native.py").read_text()
        self.assertIn("PS5VK_MULTIVIEW_INSTANCE_PROBE", build)
        self.assertIn('"first_instance": "0x07ffffff"', build)
        self.assertIn('"instance_count": 1', build)
        self.assertIn("runtime_view_index_instance.vert", build)
        generator = (ROOT / "tools/prepare_runtime_graphics.py").read_text()
        self.assertIn("runtime_view_index_instance.vert", generator)
        scene = (ROOT / "native/graphics_main.c").read_text()
        self.assertIn('"PS5VK_MULTIVIEW_VIEW_GATE mask=%08x views=%u view_index_slot=%u start_instance_slot=%u "',
                      scene)

    def test_multiview_diagnostic_stays_private(self):
        """The six-view gate is a private build switch, never a reported one."""
        for public in (ROOT / "include").rglob("*.h"):
            self.assertNotIn("PS5VK_MULTIVIEW_DIAGNOSTIC", public.read_text(),
                             f"{public} must not expose the diagnostic gate")
        self.assertIn("PS5VK_MULTIVIEW_DIAGNOSTIC",
                      (ROOT / "src/vk_internal.h").read_text())
        self.assertIn("PS5VK_MULTIVIEW_DIAGNOSTIC",
                      (ROOT / "tools/build_native.py").read_text())

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

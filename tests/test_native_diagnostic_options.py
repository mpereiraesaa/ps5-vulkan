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
    def test_shader_int16_diagnostic_has_distinct_build_identity(self):
        from tools.build_upstream_cts import tessellation_build_profile

        ordinary = tessellation_build_profile({})
        measured = tessellation_build_profile({"PS5VK_SHADER_INT16_DIAGNOSTIC": "1"})
        self.assertEqual("0", ordinary["switches"]["PS5VK_SHADER_INT16_DIAGNOSTIC"])
        self.assertFalse(ordinary["experimental"])
        self.assertEqual("1", measured["switches"]["PS5VK_SHADER_INT16_DIAGNOSTIC"])
        self.assertTrue(measured["experimental"])
        platform = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("#if defined(PS5VK_SHADER_INT16_DIAGNOSTIC) && PS5VK_SHADER_INT16_DIAGNOSTIC", platform)
        self.assertNotIn("PS5VK_SHADER_INT16_DIAGNOSTIC", (ROOT / "src/vk_device.c").read_text())
    def test_t07_promoted_switches_are_retired(self):
        from tools.build_upstream_cts import tessellation_build_profile

        names = ("PS5VK_IMAGE_CUBE_ARRAY_DIAGNOSTIC",
                 "PS5VK_TEXTURE_COMPRESSION_BC_DIAGNOSTIC",
                 "PS5VK_OCCLUSION_PRECISE_DIAGNOSTIC",
                 "PS5VK_GATHER_EXTENDED_DIAGNOSTIC",
                 "PS5VK_D16_DEPTH_ATTACHMENT_DIAGNOSTIC",
                 "PS5VK_RGBA8_INTEGER_ATTACHMENT_DIAGNOSTIC",
                 "PS5VK_D32_SAMPLED_DIAGNOSTIC")
        profile = tessellation_build_profile({name: "1" for name in names})
        self.assertFalse(profile["experimental"])
        sdk = (ROOT / "tools/build_sdk.py").read_text()
        for name in names:
            self.assertNotIn(name, profile["switches"])
            self.assertNotIn(name, sdk)
            self.assertNotIn(name, (ROOT / "native/platform_ps5.c").read_text())

    def test_promoted_fragment_feature_has_no_diagnostic_switch(self):
        platform = (ROOT / "native/platform_ps5.c").read_text()
        builder = (ROOT / "tools/build_sdk.py").read_text()
        self.assertIn("PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS", platform)
        self.assertNotIn("PS5VK_T06_DIAGNOSTIC", platform)
        self.assertNotIn("PS5VK_T06_DIAGNOSTIC", builder)
        self.assertNotIn("PS5VK_T06_DIAGNOSTIC",
                         (ROOT / "tools/build_native.py").read_text())
        for public in (ROOT / "include").rglob("*.h"):
            self.assertNotIn("PS5VK_T06_DIAGNOSTIC", public.read_text(),
                             f"{public} must not restore a retired measurement switch")

    def test_fragment_store_probe_is_bounded_and_private(self):
        self.rejected({"PS5VK_FRAGMENT_STORE_PROBE": "2"},
                      "must be 0 or 1")
        self.rejected({"PS5VK_FRAGMENT_STORE_PROBE": "1"},
                      "requires graphics API, runtime graphics and draw")
        self.rejected({"PS5VK_FRAGMENT_STORE_PROBE": "1",
                       "PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_RUNTIME_GRAPHICS": "1",
                       "PS5VK_GRAPHICS_DRAW": "1",
                       "PS5VK_GRAPHICS_WITNESSES": "1"},
                      "bounded standalone scene")
        builder = (ROOT / "tools/build_native.py").read_text()
        source = (ROOT / "native/fragment_store_probe.c").read_text()
        self.assertNotIn("PS5VK_T06_DIAGNOSTIC", builder)
        self.assertIn("control_counter=%u candidate_counter=%u", source)
        self.assertIn("control_and_candidate_same_submit", builder)

    def test_dual_source_fixture_is_owned_and_runtime_packaged(self):
        generator = (ROOT / "tools/prepare_runtime_graphics.py").read_text()
        self.assertIn('"experiments/graphics/runtime_dual_source.frag"', generator)
        self.assertIn('"dual_source_fragment"', generator)
        shader = (ROOT / "experiments/graphics/runtime_dual_source.frag").read_text()
        self.assertIn("layout(location = 0, index = 0)", shader)
        self.assertIn("layout(location = 0, index = 1)", shader)

    def test_two_mrt_probe_is_bounded_and_private(self):
        self.rejected({"PS5VK_TWO_MRT_PROBE": "2"}, "must be 0 or 1")
        self.rejected({"PS5VK_TWO_MRT_PROBE": "1"},
                      "requires graphics API, runtime graphics and draw")
        self.rejected({"PS5VK_TWO_MRT_PROBE": "1",
                       "PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_RUNTIME_GRAPHICS": "1",
                       "PS5VK_GRAPHICS_DRAW": "1",
                       "PS5VK_DUAL_SOURCE_PROBE": "1"},
                      "bounded standalone scene")
        builder = (ROOT / "tools/build_native.py").read_text()
        sdk = (ROOT / "tools/build_sdk.py").read_text()
        platform = (ROOT / "native/platform_ps5.c").read_text()
        # The two-MRT witness is still a private probe, but the capability it
        # exercised is promoted: the platform reports independentBlend in every
        # build, so no private gate has to reach it through either build path.
        self.assertIn("-DPS5VK_TWO_MRT_PROBE=", builder)
        self.assertNotIn("PS5VK_INDEPENDENT_BLEND_DIAGNOSTIC", sdk)
        self.assertNotIn("PS5VK_INDEPENDENT_BLEND_DIAGNOSTIC", platform)
        self.assertIn("PS5VK_FEATURE_INDEPENDENT_BLEND", platform)
        self.assertNotIn("PS5VK_INTEGER_TARGET_DIAGNOSTIC", sdk)
        limits = (ROOT / "src/graphics_limits.h").read_text()
        self.assertIn("maxColorAttachments=PS5VK_MAX_COLOR_ATTACHMENTS", limits)
        self.assertNotIn("PS5VK_INDEPENDENT_BLEND_DIAGNOSTIC", limits)
        self.assertIn("two_mrt_probe.c", builder)
        self.assertIn("two_mrt_oracle.c", builder)
        source = (ROOT / "native/two_mrt_probe.c").read_text()
        self.assertIn("PS5VK_TWO_MRT_READBACK", source)
        self.assertIn("ps5vk_two_mrt_verdict", source)
        generator = (ROOT / "tools/prepare_runtime_graphics.py").read_text()
        self.assertIn('"experiments/graphics/runtime_two_mrt.frag"', generator)
        self.assertIn('"two_mrt_fragment"', generator)
        shader = (ROOT / "experiments/graphics/runtime_two_mrt.frag").read_text()
        self.assertIn("layout(location = 0)", shader)
        self.assertIn("layout(location = 1)", shader)

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

    def test_occlusion_api_probe_is_bounded_to_depth_witness(self):
        self.rejected({"PS5VK_OCCLUSION_QUERY_API_PROBE": "2"},
                      "requires runtime graphics, depth-enabled probe 15")
        self.rejected({"PS5VK_OCCLUSION_QUERY_API_PROBE": "1"},
                      "requires runtime graphics, depth-enabled probe 15")
        source = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE", source)
        self.assertNotIn("PS5VK_OCCLUSION_PRECISE_DIAGNOSTIC", source)
        build = (ROOT / "tools/build_native.py").read_text()
        sdk_build = (ROOT / "tools/build_sdk.py").read_text()
        self.assertNotIn("PS5VK_OCCLUSION_PRECISE_DIAGNOSTIC", build)
        self.assertNotIn("PS5VK_OCCLUSION_PRECISE_DIAGNOSTIC", sdk_build)
        self.assertIn('scissor_probe in ("8", "13", "15") and use_runtime_graphics', build)

    def test_host_query_reset_probe_requires_sdk_query_witness(self):
        self.rejected({"PS5VK_HOST_QUERY_RESET_PROBE": "2"},
                      "requires SDK-linked occlusion query API probe")
        self.rejected({"PS5VK_HOST_QUERY_RESET_PROBE": "1"},
                      "requires SDK-linked occlusion query API probe")

    def test_gather_probe_is_sdk_linked(self):
        self.rejected({"PS5VK_GRAPHICS_API": "build/graphics/control-gxn440da",
                       "PS5VK_RUNTIME_GRAPHICS": "1",
                       "PS5VK_GRAPHICS_DRAW": "1",
                       "PS5VK_GRAPHICS_SCISSOR_PROBE": "15",
                       "PS5VK_GATHER_FORM": "1"},
                      "requires PS5VK_USE_SDK=1")
        self.rejected({"PS5VK_USE_SDK": "1", "PS5VK_GATHER_FORM": "9"},
                      "must be 0 through 8")
        self.rejected({"PS5VK_USE_SDK": "1",
                       "PS5VK_GRAPHICS_API": "build/graphics/control-gxn440da",
                       "PS5VK_RUNTIME_GRAPHICS": "1",
                       "PS5VK_GRAPHICS_DRAW": "1",
                       "PS5VK_GRAPHICS_SCISSOR_PROBE": "15",
                       "PS5VK_GATHER_FORM": "1",
                       "PS5VK_OCCLUSION_DEPTH_PROBE": "1"},
                      "requires a single runtime-graphics draw")
        platform = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("PS5VK_FEATURE_SHADER_IMAGE_GATHER_EXTENDED", platform)
        self.assertNotIn("PS5VK_GATHER_EXTENDED_DIAGNOSTIC", platform)
        device = (ROOT / "src/vk_device.c").read_text()
        self.assertIn("VkPhysicalDeviceFeatures, shaderImageGatherExtended", device)
        native_build = (ROOT / "tools/build_native.py").read_text()
        sdk_build = (ROOT / "tools/build_sdk.py").read_text()
        self.assertNotIn("PS5VK_GATHER_EXTENDED_DIAGNOSTIC", native_build)
        self.assertNotIn("PS5VK_GATHER_EXTENDED_DIAGNOSTIC", sdk_build)
        compiler = (ROOT / "src/ps5vk_compiler.c").read_text()
        self.assertIn("PS5VK_FEATURE_SHADER_IMAGE_GATHER_EXTENDED", compiler)

    def test_d16_depth_witness_is_bounded_to_probe_14_draw(self):
        self.rejected({"PS5VK_D16_DEPTH_WITNESS": "2"}, "must be 0 or 1")
        self.rejected({"PS5VK_D16_DEPTH_WITNESS": "1"},
                      "requires graphics API, draw and PS5VK_GRAPHICS_SCISSOR_PROBE=14")
        self.rejected({"PS5VK_D16_DEPTH_WITNESS": "1",
                       "PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_GRAPHICS_SCISSOR_PROBE": "14"},
                      "requires graphics API, draw and PS5VK_GRAPHICS_SCISSOR_PROBE=14")
        source = (ROOT / "native/graphics_main.c").read_text()
        builder = (ROOT / "tools/build_native.py").read_text()
        self.assertNotIn("PS5VK_D16_DEPTH_ATTACHMENT_DIAGNOSTIC", builder)
        self.assertIn('use_runtime_sdk and d16_depth_witness == "1"', builder)
        self.assertIn("PS5VK_USE_SDK=1", builder)
        self.assertIn("use_runtime_graphics or continuous == \"1\"", builder)
        self.assertIn("D16 depth witness requires the bounded non-runtime, non-presented probe-14 draw", builder)
        self.assertIn("di.format=PS5VK_D16_DEPTH_WITNESS?VK_FORMAT_D16_UNORM", source)
        self.assertIn(".usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT", source)
        self.assertIn(".loadOp=PS5VK_D16_DEPTH_WITNESS?VK_ATTACHMENT_LOAD_OP_CLEAR", source)
        self.assertIn("max_extent=128x128 mip_levels=1 layers=1 samples=1", source)

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

    def test_t09_diagnostics_are_bounded_and_graphics_only(self):
        from tools.build_upstream_cts import tessellation_build_profile

        for name in ("PS5VK_IMAGELESS_FRAMEBUFFER_DIAGNOSTIC",
                     "PS5VK_SAMPLER_MIRROR_CLAMP_DIAGNOSTIC"):
            with self.subTest(name=name):
                self.rejected({name: "1"}, "requires the graphics profile API")
                self.rejected({"PS5VK_GRAPHICS_API": "unused", name: "2"},
                              "must be 0 or 1")
                profile = tessellation_build_profile({name: "1"})
                self.assertTrue(profile["experimental"])
                self.assertEqual(profile["switches"][name], "1")
        self.rejected({"PS5VK_SAMPLER_MIRROR_CASE": "8"},
                      "requires an SDK-linked graphics build")
        self.rejected({"PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_SAMPLER_MIRROR_CASE": "20"},
                      "must be -1 or 8..19")
        sdk_case = {"PS5VK_GRAPHICS_API": "unused", "PS5VK_USE_SDK": "1",
                    "PS5VK_SAMPLER_MIRROR_CASE": "11"}
        self.rejected(sdk_case, "requires scissor probe 6, draw and no presentation")
        self.rejected({**sdk_case, "PS5VK_GRAPHICS_SCISSOR_PROBE": "6"},
                      "requires scissor probe 6, draw and no presentation")
        self.rejected({**sdk_case, "PS5VK_GRAPHICS_SCISSOR_PROBE": "6",
                       "PS5VK_GRAPHICS_DRAW": "1", "PS5VK_GRAPHICS_PRESENT": "1"},
                      "requires scissor probe 6, draw and no presentation")

    def test_sample_rate_diagnostic_is_graphics_only(self):
        """The sampleRateShading bit is SHIPPING; the switch is not its gate.

        The bit was promoted on 2026-09-23, so the platform sets it
        unconditionally and no preprocessor guard may decide it again. The
        switch itself survives as the build option the MEASUREMENT payloads
        need (the register survey and the sample-rate probe), which is why it
        is still graphics-only and still bounded to 0 or 1."""
        self.rejected({"PS5VK_SAMPLE_RATE_DIAGNOSTIC": "1"},
                      "requires the graphics profile API")
        self.rejected({"PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_SAMPLE_RATE_DIAGNOSTIC": "2"},
                      "must be 0 or 1")
        platform = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("platform->supported_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;",
                      platform)
        self.assertNotIn("#if defined(PS5VK_SAMPLE_RATE_DIAGNOSTIC)", platform)
        self.assertIn("PS5VK_SAMPLE_RATE_DIAGNOSTIC", platform)
        # The switch only decides what a MEASUREMENT build may exercise; no
        # public query is answered from the define itself.
        self.assertNotIn("PS5VK_FEATURE_SAMPLE_RATE_SHADING",
                         (ROOT / "tools/build_native.py").read_text())

    def test_sample_rate_probe_is_bounded_private_and_diagnostic_only(self):
        """The sample-rate measurement scene cannot ship or run half-built.

        It clears a multisampled colour target and reads the surface's own
        storage back, and after the 2026-09-23 promotion it measures the
        SHIPPING state: the witness oracle is compiled in unconditionally and
        the measurement switch only selects the register survey around it. The
        scene is still a bounded standalone one and cannot be combined with any
        other diagnostic."""
        self.rejected({"PS5VK_SAMPLE_RATE_PROBE": "1"},
                      "requires graphics API, runtime graphics and draw")
        self.rejected({"PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_SAMPLE_RATE_PROBE": "3"},
                      "must be 0, 1 or 2")
        builder = (ROOT / "tools/build_native.py").read_text()
        source = (ROOT / "native/sample_rate_probe.c").read_text()
        # The measurement is bounded to one multisampled colour target whose
        # storage is read back and judged; it is a standalone scene, so no
        # other diagnostic may be selected with it.
        self.assertIn("PS5VK_SAMPLE_RATE_PROBE is a bounded standalone scene", builder)
        self.assertIn("distinct == 1u && correct == words && first == expected", source)
        self.assertIn("PS5VK_SAMPLE_RATE_CLEAR extent=", source)
        # Value 2 is the same witness followed by the step walk through the CTS
        # oracle's render pass: every step is announced before it runs, so the
        # last step in the log names the call that did not return, and a
        # refusal names the step and the Vulkan result instead of dying.
        self.assertIn('"PS5VK_SAMPLE_RATE_PROBE must be 0, 1 or 2"', builder)
        self.assertIn("PS5VK_SAMPLE_RATE_SHAPE step=%s rc=%d ok=%u", source)
        self.assertIn("step=create_pipeline subpass=%u", source)
        header = (ROOT / "native/sample_rate_probe.h").read_text()
        self.assertIn("ps5vk_sample_rate_shape_probe", header)
        main = (ROOT / "native/graphics_main.c").read_text()
        self.assertIn("#if PS5VK_SAMPLE_RATE_PROBE == 2", main)
        self.assertIn("ps5vk_runtime_subpass_fetch_fragment", main)

    def test_promoted_dual_source_has_no_measurement_switch(self):
        """dualSrcBlend is advertised by the shipping platform now.

        The capability bit, the widened blend space, the partial write masks
        and the format blend cap all ship; the measurement switch that used to
        gate them is retired, exactly as the fragment-storage one was, so
        nothing can re-enable a diagnostic build of an advertised feature.
        """
        for source in ("native/platform_ps5.c", "native/runtime_graphics_compiler.c",
                       "src/texture_format.c", "tools/build_sdk.py",
                       "tools/build_native.py", "tools/build_upstream_cts.py",
                       "Makefile"):
            self.assertNotIn("PS5VK_DUAL_SOURCE_DIAGNOSTIC",
                             (ROOT / source).read_text(),
                             f"{source} must not restore the retired switch")
        platform = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("platform->supported_features |= PS5VK_FEATURE_DUAL_SRC_BLEND;",
                      platform)
        for public in (ROOT / "include").rglob("*.h"):
            self.assertNotIn("PS5VK_DUAL_SOURCE_DIAGNOSTIC", public.read_text(),
                             f"{public} must not expose the retired gate")

    def test_dual_source_witness_is_bounded_and_runnable(self):
        """The blend witness is a standalone scene; it runs on the shipping
        build now that the feature is advertised."""
        self.rejected({"PS5VK_DUAL_SOURCE_PROBE": "1"},
                      "requires graphics API, runtime graphics and draw")
        self.rejected({"PS5VK_GRAPHICS_API": "unused",
                       "PS5VK_DUAL_SOURCE_PROBE": "2"},
                      "must be 0 or 1")

    def test_dual_source_witness_oracle_and_artifact_are_pinned(self):
        probe = (ROOT / "native/dual_source_probe.c").read_text()
        for pinned in ("PS5VK_DUAL_SOURCE_READBACK",
                       "VK_BLEND_FACTOR_SRC1_COLOR",
                       "ps5vk_dual_source_verdict(observed[0], observed[1])"):
            self.assertIn(pinned, probe)
        # The verdict is the same pure predicate the host regressions exercise.
        oracle = (ROOT / "src/dual_source_oracle.h").read_text()
        self.assertIn("PS5VK_DUAL_SOURCE_BLEND_R = 51", oracle)
        self.assertIn("PS5VK_DUAL_SOURCE_MIN_DELTA", oracle)
        self.assertTrue((ROOT / "tests/test_dual_source_oracle.c").is_file())
        self.assertIn("tests/test_dual_source_oracle.c",
                      (ROOT / "Makefile").read_text())
        self.assertIn("ps5vk_dual_source_probe",
                      (ROOT / "native/graphics_main.c").read_text())
        builder = (ROOT / "tools/build_native.py").read_text()
        self.assertIn("dual_source_probe=1", builder)
        self.assertIn("dual_source_witness", builder)
        verifier = (ROOT / "tools/verify_dual_source.py").read_text()
        self.assertIn('"candidate_equation"', verifier)
        self.assertIn("candidate == wanted_candidate", verifier)
        self.assertTrue((ROOT / "tools/run_dual_source.py").is_file())
        for public in (ROOT / "include").rglob("*.h"):
            self.assertNotIn("PS5VK_DUAL_SOURCE_PROBE", public.read_text(),
                             f"{public} must not expose the witness switch")

    def test_dual_source_witness_recipe_links_the_sdk(self):
        """The witness recipe must link the SDK: the direct build does not
        define the runtime compiler, so the graphics feature block is not
        compiled and vkCreateDevice refuses the feature before the witness
        ever runs."""
        witness = subprocess.run(
            ["make", "-n", "native-dual-source", "GRAPHICS_CONTROL=fixture",
             "GLSLANG=glslang-test"], cwd=ROOT, capture_output=True, text=True,
            check=True)
        for expected in ("PS5VK_RUNTIME_GRAPHICS=1", "PS5VK_SHELL_CLOSE=1",
                         "PS5VK_GLSLANG=glslang-test", "PS5VK_GRAPHICS_DRAW=1",
                         "PS5VK_DUAL_SOURCE_PROBE=1", "PS5VK_USE_SDK=1"):
            self.assertIn(expected, witness.stdout)
        # The witness is an offscreen scene: presenting would move the artifact
        # manifest to the presentation stage, which the strict verifier refuses.
        self.assertNotIn("PS5VK_GRAPHICS_PRESENT=1", witness.stdout)
        verifier = (ROOT / "tools/verify_dual_source.py").read_text()
        self.assertIn('"graphics-api-offscreen-draw"', verifier)

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

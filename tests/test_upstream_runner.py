import base64
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest

from cts.upstream_runner import (
    parse_upstream_log_lines,
    parse_qpa_results,
    verify_upstream_acceptance,
    verify_run_identity,
    UpstreamVerificationError
)
from tools.build_upstream_cts import write_focused_storage_source

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPO_ROOT / "cts/upstream/manifest.json"

class TestUpstreamRunner(unittest.TestCase):
    def setUp(self):
        self.manifest = {
            "cases": [
                {"path": "dEQP-VK.api.smoke.create_sampler"},
                {"path": "dEQP-VK.api.smoke.create_shader"},
                {"path": "dEQP-VK.compute.basic.shared_var_single_invocation"}
            ]
        }
        self.sample_qpa = (
            "#sessionInfo releaseName 1.3.8.4\n"
            "#beginSession\n"
            "#beginTestCaseResult dEQP-VK.api.smoke.create_sampler\n"
            "<TestCaseResult CasePath=\"dEQP-VK.api.smoke.create_sampler\">\n"
            "  <Result StatusCode=\"Pass\">Creating sampler succeeded</Result>\n"
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            "#beginTestCaseResult dEQP-VK.api.smoke.create_shader\n"
            "<TestCaseResult CasePath=\"dEQP-VK.api.smoke.create_shader\">\n"
            "  <Result StatusCode=\"Pass\">Creating shader module succeeded</Result>\n"
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            "#beginTestCaseResult dEQP-VK.compute.basic.shared_var_single_invocation\n"
            "<TestCaseResult CasePath=\"dEQP-VK.compute.basic.shared_var_single_invocation\">\n"
            "  <Result StatusCode=\"Pass\">Pass</Result>\n"
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            "#endSession\n"
        ).encode("utf-8")

    def _generate_log_stream(self, qpa_bytes: bytes, chunk_size: int = 64, corrupt_chunk_idx: int = -1,
                             drop_chunk_idx: int = -1, duplicate_chunk_idx: int = -1, exit_status: int = 0):
        lines = []
        lines.append("0\t1000\tMARK\tUPSTREAM_CTS_START run_id=test-run selection_hash=abc eboot_sha256=123")

        chunks = [qpa_bytes[i:i + chunk_size] for i in range(0, len(qpa_bytes), chunk_size)]
        for seq, ch in enumerate(chunks):
            if seq == drop_chunk_idx:
                continue
            b64_str = base64.b64encode(ch).decode("ascii")
            if seq == corrupt_chunk_idx:
                b64_str = "!!!NotBase64!!!"
            lines.append(f"0\t1000\tQPA\tCHUNK seq={seq} size={len(ch)} data={b64_str}")
            if seq == duplicate_chunk_idx:
                lines.append(f"0\t1000\tQPA\tCHUNK seq={seq} size={len(ch)} data={b64_str}")

        h = hashlib.sha256(qpa_bytes).hexdigest()
        lines.append(f"0\t1000\tMARK\tUPSTREAM_CTS_END chunks={len(chunks)} total_bytes={len(qpa_bytes)} sha256={h}")
        if exit_status is not None:
            lines.append(f"0\t1000\tINFO\tUPSTREAM_CTS_COMPLETE status={exit_status}")
        return lines

    def test_valid_stream_verification(self):
        lines = self._generate_log_stream(self.sample_qpa)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        self.assertEqual(qpa_bytes, self.sample_qpa)
        self.assertEqual(meta["start"]["run_id"], "test-run")
        self.assertEqual(meta["exit_code"], 0)

        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        self.assertEqual(len(results), 3)

        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertTrue(v["report_valid"])
        self.assertTrue(v["all_required_passed"])
        self.assertTrue(v["strict_verified"])
        self.assertEqual(v["pass_count"], 3)
        self.assertEqual(len(v["missing"]), 0)
        self.assertEqual(len(v["unexpected"]), 0)

    def test_corrupted_base64_raises(self):
        lines = self._generate_log_stream(self.sample_qpa, corrupt_chunk_idx=1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("Corrupted base64", str(ctx.exception))

    def test_dropped_chunk_raises(self):
        lines = self._generate_log_stream(self.sample_qpa, drop_chunk_idx=2)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("Chunk count mismatch", str(ctx.exception))

    def test_duplicate_chunk_raises(self):
        lines = self._generate_log_stream(self.sample_qpa, duplicate_chunk_idx=1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("Duplicate chunk", str(ctx.exception))

    def test_missing_completion_raises(self):
        lines = self._generate_log_stream(self.sample_qpa, exit_status=None)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("UPSTREAM_CTS_COMPLETE marker", str(ctx.exception))

    def test_nonzero_exit_status_fails_strict(self):
        lines = self._generate_log_stream(self.sample_qpa, exit_status=1)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertFalse(v["report_valid"])
        self.assertFalse(v["strict_verified"])

    def test_not_supported_fails_strict_acceptance(self):
        qpa_unsupported = self.sample_qpa.decode("utf-8").replace(
            "<Result StatusCode=\"Pass\">Creating sampler succeeded</Result>",
            "<Result StatusCode=\"NotSupported\">Unsupported sampler format</Result>"
        ).encode("utf-8")
        lines = self._generate_log_stream(qpa_unsupported)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertTrue(v["report_valid"])  # valid stream
        self.assertFalse(v["all_required_passed"])  # but not all required passed
        self.assertFalse(v["strict_verified"])
        self.assertEqual(v["not_supported_count"], 1)
        self.assertEqual(len(v["acceptance_failures"]), 1)

    def test_missing_case_fails_strict_acceptance(self):
        # Manifest expects 3 cases, but QPA only has 2
        manifest_extra = {
            "cases": self.manifest["cases"] + [{"path": "dEQP-VK.memory.missing_test"}]
        }
        lines = self._generate_log_stream(self.sample_qpa)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(manifest_extra, meta, results)
        self.assertFalse(v["report_valid"])
        self.assertFalse(v["strict_verified"])
        self.assertIn("dEQP-VK.memory.missing_test", v["missing"])

    def test_all_unsupported_report_is_not_acceptance(self):
        """An internally valid stream where every case is unsupported is not a pass."""
        qpa = self.sample_qpa.decode("utf-8")
        qpa = qpa.replace('StatusCode="Pass"', 'StatusCode="NotSupported"')
        lines = self._generate_log_stream(qpa.encode("utf-8"))
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertTrue(v["report_valid"])
        self.assertFalse(v["all_required_passed"])
        self.assertEqual(v["not_supported_count"], 3)
        self.assertEqual(v["pass_count"], 0)

    def test_unknown_status_is_not_acceptance(self):
        qpa = self.sample_qpa.decode("utf-8").replace(
            '<Result StatusCode="Pass">Creating sampler succeeded</Result>',
            '<Result StatusCode="SomethingElse">?</Result>'
        ).encode("utf-8")
        lines = self._generate_log_stream(qpa)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertFalse(v["all_required_passed"])
        self.assertEqual(v["other_count"], 1)

    def test_run_identity_mismatch_is_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        meta, _ = parse_upstream_log_lines(lines)
        # Matching identity passes.
        verify_run_identity(meta, {"run_id": "test-run", "selection_hash": "abc",
                                   "eboot_sha256": "123"})
        with self.assertRaises(UpstreamVerificationError) as ctx:
            verify_run_identity(meta, {"selection_hash": "different"})
        self.assertIn("selection_hash", str(ctx.exception))
        with self.assertRaises(UpstreamVerificationError):
            verify_run_identity(meta, {"eboot_sha256": "stale-binary"})

    # --- structural (fail-closed) parsing -----------------------------------

    def _parse(self, qpa_text):
        lines = self._generate_log_stream(qpa_text.encode("utf-8"))
        metadata, qpa_bytes = parse_upstream_log_lines(lines)
        return parse_qpa_results(qpa_bytes.decode("utf-8"))

    def test_incomplete_xml_is_rejected(self):
        """A missing closing tag must not be salvaged into a Pass status."""
        qpa = self.sample_qpa.decode("utf-8").replace("</TestCaseResult>", "", 1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("never closed", str(ctx.exception))

    def test_multiple_result_elements_are_rejected(self):
        qpa = self.sample_qpa.decode("utf-8").replace(
            '<Result StatusCode="Pass">Creating sampler succeeded</Result>',
            '<Result StatusCode="Pass">a</Result><Result StatusCode="Fail">b</Result>',
            1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("exactly one <Result>", str(ctx.exception))

    def test_case_path_mismatch_is_rejected(self):
        qpa = self.sample_qpa.decode("utf-8").replace(
            'CasePath="dEQP-VK.api.smoke.create_sampler"',
            'CasePath="dEQP-VK.api.smoke.something_else"', 1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("CasePath", str(ctx.exception))

    def test_missing_session_markers_are_rejected(self):
        qpa = (self.sample_qpa.decode("utf-8")
               .replace("#beginSession\n", "")
               .replace("#endSession\n", ""))
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("#beginSession", str(ctx.exception))

    def test_unterminated_final_case_is_rejected(self):
        # Drop the terminator of the last case: the session must not accept it.
        qpa = self.sample_qpa.decode("utf-8").replace(
            "#endTestCaseResult\n#endSession", "#endSession")
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("no terminator", str(ctx.exception))

    def test_terminated_case_is_not_a_pass(self):
        """#terminateTestCaseResult must surface as a failure, never disappear."""
        qpa = (
            "#beginSession\n"
            "#beginTestCaseResult dEQP-VK.api.smoke.create_sampler\n"
            '<TestCaseResult CasePath="dEQP-VK.api.smoke.create_sampler">\n'
            '  <Result StatusCode="Pass">ok</Result>\n'
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            "#beginTestCaseResult dEQP-VK.api.smoke.create_shader\n"
            '<TestCaseResult CasePath="dEQP-VK.api.smoke.create_shader">\n'
            "#terminateTestCaseResult Crash\n"
            "#endSession\n"
        )
        results = self._parse(qpa)
        terminated = [r for r in results if r["case_path"].endswith("create_shader")]
        self.assertEqual(len(terminated), 1)
        self.assertEqual(terminated[0]["status"], "Crash")
        self.assertTrue(terminated[0]["terminated"])

    def test_case_timing_section_is_tolerated(self):
        qpa = self.sample_qpa.decode("utf-8").replace(
            "#beginSession\n",
            "#beginSession\n#beginTestsCasesTime\n"
            "<TestsCasesTime></TestsCasesTime>\n#endTestsCasesTime\n", 1)
        self.assertEqual(len(self._parse(qpa)), 3)

    def test_empty_selection_is_rejected(self):
        """An empty manifest must never satisfy acceptance vacuously."""
        lines = self._generate_log_stream(self.sample_qpa)
        metadata, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        verification = verify_upstream_acceptance({"cases": []}, metadata, results)
        self.assertFalse(verification["report_valid"])
        self.assertFalse(verification["strict_verified"])
        self.assertTrue(verification["policy_failures"])

    # --- stream-level marker integrity --------------------------------------

    def test_duplicate_start_marker_is_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        lines.insert(1, lines[0])
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("UPSTREAM_CTS_START", str(ctx.exception))

    def test_duplicate_completion_marker_is_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        lines.append(lines[-1])
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("UPSTREAM_CTS_COMPLETE", str(ctx.exception))

    def test_out_of_order_markers_are_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        complete = lines.pop()
        end_index = next(i for i, line in enumerate(lines) if "UPSTREAM_CTS_END" in line)
        lines.insert(end_index, complete)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("out of order", str(ctx.exception))

    def test_chunk_outside_marker_window_is_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        chunk = lines.pop(1)
        lines.append(chunk)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("outside the start/end marker window", str(ctx.exception))

    def test_selection_manifest_is_wellformed(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        self.assertEqual(manifest["manifest_version"], "1.0")
        self.assertEqual(manifest["cts_pin"]["tag"], "vulkan-cts-1.3.8.4")
        self.assertEqual(manifest["cts_pin"]["commit"],
                         "a0270c1897597e6c77679870e10415398a13001c")
        self.assertFalse(manifest["acceptance_policy"]["allow_not_supported"])
        self.assertFalse(manifest["acceptance_policy"]["allow_skip"])

        pins = manifest["external_pins"]
        for name in ("glslang", "spirv-tools", "spirv-headers", "amber"):
            self.assertRegex(pins[name], r"^[0-9a-f]{40}$", name)

        paths = [case["path"] for case in manifest["cases"]]
        self.assertEqual(len(paths), len(set(paths)), "duplicate case path in manifest")
        self.assertTrue(paths, "manifest must select at least one case")
        for case in manifest["cases"]:
            self.assertTrue(case["path"].startswith("dEQP-VK."), case["path"])
            self.assertTrue(case["source"], case["path"])
            self.assertEqual(case["expected_status"], "Pass", case["path"])
            self.assertIn("rationale", case)

        # The frozen list must not be the retired synthetic contract suite.
        self.assertFalse(any("contract." in p for p in paths))

    def test_physical_device_reporting_uses_original_upstream_oracles(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        accepted = {case["path"]: case for case in manifest["cases"]}
        diagnostics = {case["path"]: case
                       for case in manifest.get("diagnostics", [])}
        self.assertIn("dEQP-VK.info.physical_devices", accepted)
        self.assertIn("dEQP-VK.info.device_queue_family_properties", accepted)
        self.assertIn("dEQP-VK.info.device_properties", diagnostics)
        self.assertIn("dEQP-VK.info.device_memory_properties", diagnostics)
        self.assertEqual("Fail", diagnostics[
            "dEQP-VK.info.device_properties"]["expected_status"])
        self.assertIn("HOST_COHERENT", diagnostics[
            "dEQP-VK.info.device_memory_properties"]["rationale"])

        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (REPO_ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn("createFeatureInfoInstanceTests", package)
        self.assertIn("createFeatureInfoDeviceTests", package)
        self.assertIn("vktApiFeatureInfo.cpp", builder)

    RESOURCE_CASES = {
        "dEQP-VK.compute.basic.ubo_to_ssbo_single_invocation",
        "dEQP-VK.compute.basic.ubo_to_ssbo_multiple_groups",
        "dEQP-VK.api.buffer_view.access.uniform_texel_buffer.r32_uint",
        "dEQP-VK.binding_model.shader_access.primary_cmd_buf.bind.storage_buffer.compute."
        "multiple_descriptor_sets.single_descriptor.offset_view_zero",
        "dEQP-VK.binding_model.shader_access.primary_cmd_buf.bind.uniform_buffer.compute."
        "multiple_descriptor_sets.single_descriptor.offset_view_zero",
    }

    PUSH_SPECIALIZATION_CASES = {
        "dEQP-VK.pipeline.push_constant.compute_pipeline.simple_test",
        "dEQP-VK.api.pipeline.pipeline_layout.lifetime.destroy_after_end",
    }

    DEFERRED_SPIRV13_SPECIALIZATION_CASES = {
        "dEQP-VK.pipeline.spec_constant.compute.basic.bool",
        "dEQP-VK.pipeline.spec_constant.compute.basic.int",
        "dEQP-VK.pipeline.spec_constant.compute.basic.uint",
        "dEQP-VK.pipeline.spec_constant.compute.basic.float",
    }

    STORAGE_WIDTH_CASES = {
        "dEQP-VK.spirv_assembly.instruction.compute.8bit_storage."
        "storagebuffer_32_to_8.storage_buffer_scalar_sint",
        "dEQP-VK.spirv_assembly.instruction.compute.8bit_storage."
        "storagebuffer_32_to_8.storage_buffer_scalar_uint",
        "dEQP-VK.spirv_assembly.instruction.compute.8bit_storage."
        "storagebuffer_32_to_8.storage_buffer_vector_sint",
        "dEQP-VK.spirv_assembly.instruction.compute.8bit_storage."
        "storagebuffer_32_to_8.storage_buffer_vector_uint",
        "dEQP-VK.spirv_assembly.instruction.compute.16bit_storage."
        "uniform_32_to_16.uniform_buffer_block_scalar_sint",
        "dEQP-VK.spirv_assembly.instruction.compute.16bit_storage."
        "uniform_32_to_16.uniform_buffer_block_scalar_uint",
        "dEQP-VK.spirv_assembly.instruction.compute.16bit_storage."
        "uniform_32_to_16.uniform_buffer_block_vector_sint",
        "dEQP-VK.spirv_assembly.instruction.compute.16bit_storage."
        "uniform_32_to_16.uniform_buffer_block_vector_uint",
        "dEQP-VK.spirv_assembly.instruction.compute.16bit_storage."
        "uniform_16_to_32.uniform_buffer_block_scalar_sint",
    }

    SYNCHRONIZATION_CASES = {
        "dEQP-VK.compute.basic.ssbo_cmd_barrier_single",
        "dEQP-VK.compute.basic.ssbo_cmd_barrier_multiple",
        "dEQP-VK.spirv_assembly.instruction.compute.workgroup_memory.uint32",
        "dEQP-VK.synchronization.basic.fence.multi_waitall_false",
        "dEQP-VK.synchronization.basic.fence.one_signaled",
        "dEQP-VK.synchronization.basic.fence.multiple_signaled",
    }

    NONCOHERENT_RANGE_CASES = {
        "dEQP-VK.memory.mapping.suballocation.full.257.flush",
        "dEQP-VK.memory.mapping.suballocation.full.257.invalidate",
        "dEQP-VK.memory.mapping.suballocation.sub.4087.offset_129.size_1025.subflush",
        "dEQP-VK.memory.mapping.suballocation.sub.4087.offset_129.size_1025.subinvalidate",
    }

    FIXED_FUNCTION_CASES = {
        "dEQP-VK.api.smoke.triangle",
    }

    def test_fixed_function_selection_uses_original_pixel_oracle(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        by_path = {case["path"]: case for case in manifest["cases"]}
        self.assertTrue(self.FIXED_FUNCTION_CASES <= set(by_path))
        case = by_path["dEQP-VK.api.smoke.triangle"]
        self.assertEqual("fixed-function-graphics", case["category"])
        self.assertEqual("Pass", case["expected_status"])
        self.assertEqual([], case["features_required"])

        source = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/"
                  "vulkan/api/vktApiSmokeTests.cpp")
        if source.exists():
            text = source.read_text(encoding="utf-8")
            self.assertIn("tcu::TestStatus renderTriangleTest", text)
            self.assertIn("vk.cmdDraw(*cmdBuf, 3u, 1u, 0u, 0u)", text)
            self.assertIn("copyImageToBuffer(vk, *cmdBuf, *image", text)
            self.assertIn("intThresholdPositionDeviationCompare", text)

        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (REPO_ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn("createSmokeTests", package)
        self.assertIn("vktApiSmokeTests.cpp", builder)

    def test_synchronization_selection_keeps_original_upstream_oracles(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        by_path = {case["path"]: case for case in manifest["cases"]}
        self.assertTrue(self.SYNCHRONIZATION_CASES <= set(by_path))
        for path in self.SYNCHRONIZATION_CASES:
            self.assertEqual("synchronization", by_path[path]["category"])
            self.assertEqual("Pass", by_path[path]["expected_status"])

        basic_path = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/"
                      "vulkan/compute/vktComputeBasicComputeShaderTests.cpp")
        workgroup_path = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/"
                          "vulkan/spirv_assembly/vktSpvAsmWorkgroupMemoryTests.cpp")
        # The large pinned CTS checkout is deliberately absent from the normal
        # GitHub host runner.  When present, freeze the original bodies/oracles;
        # otherwise the manifest provenance and package/build registration below
        # remain mandatory instead of turning checkout absence into a false red.
        if basic_path.exists() and workgroup_path.exists():
            basic = basic_path.read_text()
            self.assertIn("class SSBOBarrierTestInstance", basic)
            self.assertIn("VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT", basic)
            self.assertIn("VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT", basic)
            self.assertIn("VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT", basic)
            self.assertIn("atomicAdd", basic)

            workgroup = workgroup_path.read_text()
            self.assertIn("OpExecutionMode %main LocalSize 16 4 2", workgroup)
            self.assertIn("OpMemoryBarrier", workgroup)
            self.assertIn("OpControlBarrier", workgroup)
            self.assertIn('SpvAsmComputeShaderCase(testCtx, "uint32", spec)', workgroup)
        else:
            for path in self.SYNCHRONIZATION_CASES:
                self.assertTrue(by_path[path]["source"].startswith("external/vulkancts/"))

        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (REPO_ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn("createBasicComputeShaderTests", package)
        self.assertIn("createWorkgroupMemoryComputeGroup", package)
        self.assertIn("vktSpvAsmWorkgroupMemoryTests.cpp", builder)

    def test_noncoherent_range_cases_are_original_and_not_gpu_evidence(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        by_path = {case["path"]: case for case in manifest["cases"]}
        self.assertTrue(self.NONCOHERENT_RANGE_CASES <= set(by_path))
        for path in self.NONCOHERENT_RANGE_CASES:
            self.assertEqual("host-memory", by_path[path]["category"])
            self.assertTrue(by_path[path]["source"].startswith(
                "external/vulkancts/modules/vulkan/memory/vktMemoryMappingTests.cpp:"))
            self.assertNotIn("GPU", by_path[path]["rationale"].replace(
                "not a GPU visibility claim", ""))

        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (REPO_ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn("createMappingTests", package)
        self.assertIn("vktMemoryMappingTests.cpp", builder)

    def test_resource_family_cannot_be_silently_removed(self):
        """The resource expansion cases are part of the frozen acceptance set."""
        manifest = json.loads(MANIFEST_PATH.read_text())
        by_path = {case["path"]: case for case in manifest["cases"]}
        missing = sorted(self.RESOURCE_CASES - set(by_path))
        self.assertEqual([], missing, "resource cases dropped from the selection")
        for path in sorted(self.RESOURCE_CASES):
            self.assertEqual(by_path[path]["category"], "resource", path)
            self.assertTrue(by_path[path]["source"], path)
            self.assertTrue(by_path[path]["rationale"], path)

    def test_resource_module_sources_are_linked(self):
        """A selected family is only real if its upstream module is compiled in."""
        build = (REPO_ROOT / "tools/build_upstream_cts.py").read_text(encoding="utf-8")
        for name in ("vktApiBufferViewAccessTests.cpp",
                     "vktApiBufferAndImageAllocationUtil.cpp",
                     "vktImageTestsUtil.cpp",
                     "vktBindingShaderAccessTests.cpp"):
            self.assertIn(name, build, f"{name} is not compiled into the payload")

    def test_push_specialization_selection_is_frozen_and_bounded(self):
        """Keep the audited Vulkan 1.0 cases and reject unsupported variants."""
        manifest = json.loads(MANIFEST_PATH.read_text())
        paths = {case["path"] for case in manifest["cases"]}
        self.assertTrue(self.PUSH_SPECIALIZATION_CASES <= paths)
        self.assertFalse(self.DEFERRED_SPIRV13_SPECIALIZATION_CASES & paths)
        self.assertFalse(any("local_size" in path.lower() for path in paths))
        self.assertFalse(any("work_group_size" in path.lower() for path in paths))

    def test_push_specialization_upstream_factories_are_linked(self):
        """A selected leaf must retain its original upstream factory and body."""
        build = (REPO_ROOT / "tools/build_upstream_cts.py").read_text(encoding="utf-8")
        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text(encoding="utf-8")
        self.assertIn("vktPipelinePushConstantTests.cpp", build)
        self.assertIn("vktApiPipelineTests.cpp", build)
        self.assertNotIn("vktPipelineSpecConstantTests.cpp", build)
        self.assertIn("createPushConstantTests", package)
        self.assertIn("createPipelineTests", package)

    def test_storage_width_selection_is_exact_and_storage_buffer_only(self):
        """Freeze the audited extension-era 8/16-bit storage subset."""
        manifest = json.loads(MANIFEST_PATH.read_text())
        by_path = {case["path"]: case for case in manifest["cases"]}
        selected = {path for path in by_path if ".8bit_storage." in path or
                    ".16bit_storage." in path}
        self.assertEqual(self.STORAGE_WIDTH_CASES, selected)
        for path in sorted(selected):
            features = by_path[path]["features_required"]
            self.assertEqual(1, len(features), path)
            self.assertIn(features[0],
                          {"storageBuffer8BitAccess", "storageBuffer16BitAccess"})
            self.assertNotIn("shaderInt8", features, path)
            self.assertNotIn("shaderInt16", features, path)

    def test_storage_width_upstream_factories_are_linked(self):
        build = (REPO_ROOT / "tools/build_upstream_cts.py").read_text(encoding="utf-8")
        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text(encoding="utf-8")
        self.assertIn("storage8_focus.cpp", build)
        self.assertIn("storage16_focus.cpp", build)
        self.assertIn("vktSpvAsmComputeShaderCase.cpp", build)
        storage8 = (REPO_ROOT / "cts/upstream/storage8_focus.cpp").read_text(encoding="utf-8")
        storage16 = (REPO_ROOT / "cts/upstream/storage16_focus.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "vktSpvAsm8bitStorageTests.cpp"', storage8)
        self.assertIn('#include "vktSpvAsm16bitStorageTests.cpp"', storage16)
        self.assertIn("addCompute8bitStorage32To8Group", storage8)
        self.assertIn("addCompute8bitStorageBuffer8To8Group", storage8)
        self.assertIn("addCompute16bitStorageUniform32To16Group", storage16)
        self.assertIn("addCompute16bitStorageUniform16To32Group", storage16)
        self.assertIn("createFocused8BitStorageComputeGroup", package)
        self.assertIn("createFocused16BitStorageComputeGroup", package)

    def test_coherent_storage_stress_case_is_diagnostic_not_acceptance(self):
        """Do not invent HOST_COHERENT support to promote a shader-adjacent case."""
        manifest = json.loads(MANIFEST_PATH.read_text())
        path = ("dEQP-VK.spirv_assembly.instruction.compute.8bit_storage."
                "uniform_8_to_8.stress_test")
        self.assertNotIn(path, {case["path"] for case in manifest["cases"]})
        diagnostics = {case["path"]: case for case in manifest["diagnostics"]}
        self.assertEqual("NotSupported", diagnostics[path]["expected_status"])
        self.assertIn("HOST_COHERENT", diagnostics[path]["rationale"])

    def test_storage_width_source_pruning_preserves_upstream_body(self):
        """The build may gate registration only, never rewrite CTS bodies."""
        needle = (
            "group->addChild(new SpvAsmComputeShaderCase(testCtx, "
            "testName.c_str(), spec));"
        )
        source_text = (
            "static const char *shader = \"OpStore %dst %value\";\n"
            f"{needle}\n"
            "verifyOutputBuffers(inputs, outputs);\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "upstream.cpp"
            destination = Path(directory) / "focused" / "upstream.cpp"
            source.write_text(source_text, encoding="utf-8")
            write_focused_storage_source(
                source, destination, ("scalar_sint", "vector_uint"), 1)
            generated = destination.read_text(encoding="utf-8")

        self.assertIn('testName == "scalar_sint"', generated)
        self.assertIn('testName == "vector_uint"', generated)
        self.assertEqual(1, generated.count(needle))
        self.assertIn('"OpStore %dst %value"', generated)
        self.assertIn("verifyOutputBuffers(inputs, outputs);", generated)

    def test_storage_width_source_pruning_fails_closed_on_upstream_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "upstream.cpp"
            destination = Path(directory) / "focused.cpp"
            source.write_text("// upstream registration changed\n", encoding="utf-8")
            with self.assertRaises(SystemExit):
                write_focused_storage_source(
                    source, destination, ("scalar_sint",), 1)
            self.assertFalse(destination.exists())

    def test_repaired_resource_cases_are_promoted_not_left_as_diagnostics(self):
        """The two repaired upstream cases must remain in strict acceptance."""
        manifest = json.loads(MANIFEST_PATH.read_text())
        accepted = {case["path"] for case in manifest["cases"]}
        diagnostics = {case["path"] for case in manifest.get("diagnostics", [])}
        repaired = {
            "dEQP-VK.api.buffer_view.access.uniform_texel_buffer.r32_uint",
            "dEQP-VK.binding_model.shader_access.primary_cmd_buf.bind.uniform_buffer.compute."
            "multiple_descriptor_sets.single_descriptor.offset_view_zero",
        }
        self.assertEqual(repaired, repaired & accepted)
        self.assertFalse(repaired & diagnostics)

    def test_every_selected_family_is_registered_by_the_package(self):
        """The integration must register the first group of every selected case."""
        integration = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text(encoding="utf-8")
        manifest = json.loads(MANIFEST_PATH.read_text())
        for case in manifest["cases"]:
            group = case["path"].split(".")[1]
            self.assertRegex(integration, r'"' + re.escape(group) + r'"',
                             f"{case['path']}: group {group!r} is not registered")

    def test_packaged_case_list_matches_the_manifest(self):
        """The packaged list and selection hash must derive from the manifest."""
        manifest = json.loads(MANIFEST_PATH.read_text())
        expected = "\n".join(case["path"] for case in manifest["cases"]) + "\n"
        expected_hash = hashlib.sha256(expected.encode("utf-8")).hexdigest()

        dist = REPO_ROOT / "dist-upstream-cts/PPSA99994"
        if not dist.is_dir():
            self.skipTest("payload not built; packaged case list not available")
        self.assertEqual((dist / "cases.txt").read_text(encoding="utf-8"), expected)
        self.assertEqual((dist / "selection_hash.txt").read_text(encoding="utf-8").strip(),
                         expected_hash)

    if __name__ == "__main__":
        unittest.main()

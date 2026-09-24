import base64
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

from cts.upstream_runner import (
    parse_upstream_log_lines,
    parse_qpa_results,
    verify_upstream_acceptance,
    verify_run_identity,
    UpstreamVerificationError
)
import tools.check_upstream_selection as upstream_selection
from tools.build_upstream_cts import (
    write_focused_buffer_copy_source,
    write_focused_robust_buffer_source,
    write_focused_storage_source,
)
from tools.check_upstream_selection import (
    _copy_and_blit_simple_image_leaf_names,
    _dynamic_state_compute_generated_segments,
    _fill_update_generated_leaf_names,
    _source_function_at_line,
)

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
        mandatory = accepted["dEQP-VK.info.device_mandatory_features"]
        self.assertEqual(["robustBufferAccess"], mandatory["features_required"])
        self.assertEqual("physical-device-features", mandatory["category"])
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
        generated = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/"
                     "framework/vulkan/generated/vulkan/vkMandatoryFeatures.inl")
        if generated.exists():
            self.assertIn("coreFeatures.features.robustBufferAccess == VK_FALSE",
                          generated.read_text(encoding="utf-8"))

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

    ROBUST_BUFFER_CASES = {
        f"dEQP-VK.robustness.buffer_access.compute.scalar_copy.r32_uint.{access}.range_{size}"
        for access in ("oob_storage_read", "oob_storage_write", "oob_uniform_read")
        for size in ("1_byte", "3_bytes", "4_bytes", "32_bytes")
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
        "dEQP-VK.synchronization.basic.event.host_set_reset",
        "dEQP-VK.synchronization.basic.event.device_set_reset",
        "dEQP-VK.synchronization.basic.event.single_submit_multi_command_buffer",
        "dEQP-VK.synchronization.basic.event.multi_submit_multi_command_buffer",
        "dEQP-VK.synchronization.basic.binary_semaphore.one_queue",
        "dEQP-VK.synchronization.basic.binary_semaphore.chain",
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

    DYNAMIC_STATE_CORE_SETTERS = {
        "line_width",
        "depth_bias",
        "blend_constants",
        "depth_bounds",
        "stencil_compare_mask",
        "stencil_write_mask",
        "stencil_reference",
    }

    def test_dynamic_state_compute_transfer_selection_is_exact_and_original(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        selected = {
            case["path"]: case for case in manifest["cases"]
            if ".dynamic_state.monolithic.compute_transfer." in case["path"]
        }
        prefix = "dEQP-VK.dynamic_state.monolithic.compute_transfer"
        expected = {
            f"{prefix}.single.{operation}.{state}.{moment}"
            for operation in ("compute", "transfer")
            for state in self.DYNAMIC_STATE_CORE_SETTERS
            for moment in ("before", "after")
        }
        expected |= {
            f"{prefix}.multi.{operation}.{moment}"
            for operation in ("compute", "transfer")
            for moment in ("before", "after")
        }
        self.assertEqual(expected, set(selected))
        self.assertEqual(32, len(selected))
        for case in selected.values():
            self.assertEqual("dynamic-state-non-interference", case["category"])
            self.assertEqual("Pass", case["expected_status"])
            self.assertEqual([], case["features_required"])
            source_line = "1264" if ".multi." in case["path"] else "1227"
            self.assertEqual(
                "external/vulkancts/modules/vulkan/dynamic_state/"
                f"vktDynamicStateComputeTests.cpp:{source_line}", case["source"])

        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (REPO_ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn("createDynamicStateComputeTests", package)
        self.assertIn('"monolithic"', package)
        self.assertIn("cleanupDynamicStateGroup", package)
        self.assertIn("vkt::DynamicState::cleanupDevice()", package)
        self.assertIn("vktDynamicStateComputeTests.cpp", builder)
        self.assertNotIn("focused_sources / \"vktDynamicStateComputeTests.cpp\"", builder)

        source = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/"
                  "vulkan/dynamic_state/vktDynamicStateComputeTests.cpp")
        if source.exists():
            text = source.read_text(encoding="utf-8")
            # Pin both original operation bodies and their result oracles.
            self.assertIn("DynamicStateComputeInstance::iterateTransfer", text)
            self.assertIn("vkd.cmdCopyBuffer", text)
            self.assertIn("if (orig != res)", text)
            self.assertIn("DynamicStateComputeInstance::iterateCompute", text)
            self.assertIn("vkd.cmdDispatch(cmdBuffer, 1u, 1u, 1u)", text)
            self.assertIn("if (bufferData[idx] != 1u)", text)
            self.assertIn("createDynamicStateComputeTests", text)
            self.assertIn("if (dynamicStateList[stateIdx] == "
                          "VK_DYNAMIC_STATE_STENCIL_REFERENCE)", text)

    def test_dynamic_state_generated_segments_are_fail_closed(self):
        source = r'''
        const VkDynamicState dynamicStateList[] = {
            VK_DYNAMIC_STATE_LINE_WIDTH,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        };
        auto prefixLen = strlen("VK_DYNAMIC_STATE_");
        auto name = de::toLower(fullName.substr(prefixLen));
        auto stateName = getDynamicStateBriefName(state);
        '''
        self.assertEqual(
            {"line_width", "stencil_reference"},
            _dynamic_state_compute_generated_segments(source))
        self.assertEqual(set(), _dynamic_state_compute_generated_segments(
            source.replace("getDynamicStateBriefName(state)", "other(state)")))
        self.assertEqual(set(), _dynamic_state_compute_generated_segments(
            source.replace("de::toLower(fullName.substr(prefixLen))", "fullName")))

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
        event_path = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/"
                      "vulkan/synchronization/vktSynchronizationBasicEventTests.cpp")
        semaphore_path = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/"
                          "vulkan/synchronization/vktSynchronizationBasicSemaphoreTests.cpp")
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

            event = event_path.read_text()
            self.assertIn('"host_set_reset"', event)
            self.assertIn('"device_set_reset"', event)
            self.assertIn('"single_submit_multi_command_buffer"', event)
            self.assertIn('"multi_submit_multi_command_buffer"', event)
            semaphore = semaphore_path.read_text()
            self.assertIn('"one_queue" + createName', semaphore)
            self.assertIn('"chain"', semaphore)
        else:
            for path in self.SYNCHRONIZATION_CASES:
                self.assertTrue(by_path[path]["source"].startswith("external/vulkancts/"))

        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (REPO_ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn("createBasicComputeShaderTests", package)
        self.assertIn("createWorkgroupMemoryComputeGroup", package)
        self.assertIn("createBasicEventTests", package)
        self.assertIn("createBasicBinarySemaphoreTests", package)
        self.assertIn("vktSpvAsmWorkgroupMemoryTests.cpp", builder)
        self.assertIn("vktSynchronizationBasicEventTests.cpp", builder)
        self.assertIn("vktSynchronizationBasicSemaphoreTests.cpp", builder)

    def test_synchronization_evidence_digests_do_not_drift(self):
        """Keep the historical synchronization evidence tied to its receipts."""

        upstream = (REPO_ROOT / "UPSTREAM_CTS.md").read_text(encoding="utf-8")
        upstream = upstream.split(
            "### Binary semaphore and event expansion (2026-09-13)", 1)[1]
        upstream = upstream.split("\n### ", 1)[0]
        validation = (REPO_ROOT / "VALIDATION.md").read_text(encoding="utf-8")
        validation = validation.split(
            "### Vulkan 1.0 binary semaphores and events", 1)[1]
        validation = validation.split("\n## ", 1)[0]
        digest_pattern = r"Selection SHA-256:\s*`([0-9a-f]{64})`"
        upstream_match = re.search(digest_pattern, upstream)
        validation_match = re.search(digest_pattern, validation)
        self.assertIsNotNone(upstream_match)
        self.assertIsNotNone(validation_match)
        selection_hash = upstream_match.group(1)
        self.assertEqual(selection_hash, validation_match.group(1))

        receipts = REPO_ROOT / "private-captures/events-semaphores"
        if not receipts.is_dir():
            return

        upstream_runs = [
            json.loads((receipts / f"upstream-run{run}.json").read_text())
            for run in (1, 2)
        ]
        executable_hashes = {
            run["metadata"]["start"]["eboot_sha256"] for run in upstream_runs
        }
        selection_hashes = {
            run["metadata"]["start"]["selection_hash"] for run in upstream_runs
        }
        qpa_hashes = {run["metadata"]["qpa_sha256"] for run in upstream_runs}
        self.assertEqual(1, len(executable_hashes))
        self.assertEqual({selection_hash}, selection_hashes)
        for digest in executable_hashes | selection_hashes | qpa_hashes:
            self.assertIn(digest, upstream)
            self.assertIn(digest, validation)

        consumer_runs = [
            json.loads((receipts / f"consumer-run{run}.json").read_text())
            for run in (1, 2)
        ]
        consumer_hashes = {run["deployment_self_sha256"] for run in consumer_runs}
        self.assertEqual(1, len(consumer_hashes))
        self.assertIn(next(iter(consumer_hashes)), validation)

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

    def test_multisample_factory_is_registered_and_compiled(self):
        """The sampleRateShading family must be addressable before it is selected.

        The dual-source window measured what a missing registration costs: dEQP
        reported 306 cases while cases.txt held 404, because a selected leaf whose
        factory was never registered is silently dropped. Every leaf of the
        multisample family gates on DEVICE_CORE_FEATURE_SAMPLE_RATE_SHADING, so
        the factory that the sample-rate selection will name has to be registered
        under the same monolithic construction group the pinned listing names its
        leaves with - and the modules the factory itself composes have to be
        compiled, or it links against factories that do not exist. This test pins
        the registration and the compilation; the selection itself is the next
        slice and lives in the manifest."""
        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text(encoding="utf-8")
        build = (REPO_ROOT / "tools/build_upstream_cts.py").read_text(encoding="utf-8")
        self.assertIn('#include "vktPipelineMultisampleTests.hpp"', package)
        self.assertIn("createMultisampleTests", package)
        for name in ("vktPipelineMultisampleTests.cpp",
                     "vktPipelineMultisampleImageTests.cpp",
                     "vktPipelineMultisampleShaderFragmentMaskTests.cpp",
                     "vktPipelineMultisampledRenderToSingleSampledTests.cpp",
                     "vktPipelineMultisampleResolveRenderAreaTests.cpp",
                     "vktPipelineMultisampleSampleLocationsExtTests.cpp",
                     "vktPipelineMultisampleMixedAttachmentSamplesTests.cpp",
                     "vktPipelineSampleLocationsUtil.cpp"):
            self.assertIn(name, build, f"{name} is not compiled into the payload")
        # The factory is added to the monolithic group, which is what makes a
        # leaf's path read pipeline.monolithic.multisample... in cases.txt.
        monolithic = package.index("createBlendTests")
        multisample = package.index("createMultisampleTests")
        self.assertLess(monolithic, multisample)
        self.assertIn("PIPELINE_CONSTRUCTION_TYPE_MONOLITHIC",
                      package[monolithic:multisample])

    def test_buffer_transfer_selection_and_diagnostics_are_explicit(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        accepted = {case["path"] for case in manifest["cases"]}
        diagnostics = {case["path"]: case for case in manifest["diagnostics"]}
        required = {
            "dEQP-VK.api.copy_and_blit.core.buffer_to_buffer.partial",
            "dEQP-VK.api.copy_and_blit.core.buffer_to_buffer.regions",
            "dEQP-VK.api.copy_and_blit.core.buffer_to_buffer.unaligned_regions",
            "dEQP-VK.api.copy_and_blit.core.buffer_to_buffer.whole",
            "dEQP-VK.api.fill_and_update_buffer.suballocation.fill_buffer_whole",
            "dEQP-VK.api.fill_and_update_buffer.suballocation.update_buffer_whole",
        }
        self.assertTrue(required <= accepted)
        for path in (
            "dEQP-VK.api.fill_and_update_buffer.dedicated_alloc.update_buffer_second_part",
            "dEQP-VK.api.fill_and_update_buffer.dedicated_alloc."
            "fill_buffer_vk_whole_size_3_extra_bytes_offset_12",
        ):
            self.assertNotIn(path, accepted)
            self.assertEqual("NotSupported", diagnostics[path]["expected_status"])
            self.assertIn("VK_KHR_dedicated_allocation",
                          diagnostics[path]["features_required"])

    def test_buffer_transfer_upstream_factories_are_linked(self):
        build = (REPO_ROOT / "tools/build_upstream_cts.py").read_text()
        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text()
        self.assertIn("vktApiCopiesAndBlittingTests.cpp", build)
        self.assertIn("vktApiFillBufferTests.cpp", build)
        self.assertIn("createCopiesAndBlittingTests", package)
        self.assertIn("createFillAndUpdateBufferTests", package)

    def test_image_copy_selection_uses_original_simple_oracles(self):
        """Keep exactly the RGBA8_UNORM simple-case oracles and nothing wider."""
        required = {
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests."
            "partial_image_pot_same_format_clear",
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests."
            "partial_image_pot_same_format_noclear",
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests."
            "partial_image_npot_same_format_clear",
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests."
            "partial_image_npot_same_format_noclear",
        }
        manifest = json.loads(MANIFEST_PATH.read_text())
        accepted = {case["path"] for case in manifest["cases"]}
        self.assertTrue(required <= accepted)
        # UINT, mixed-format, depth and stencil leaves are outside the
        # advertised RGBA8 UNORM transfer role and must not be selected.
        for path in (
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests.whole_image",
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests.partial_image",
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests.depth",
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests.stencil",
        ):
            self.assertNotIn(path, accepted)
        self.assertFalse(any("diff_format" in path for path in accepted))

        upstream = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/"
                    "modules/vulkan/api/vktApiCopiesAndBlittingTests.cpp")
        if not upstream.is_file():
            return
        text = upstream.read_text(encoding="utf-8")
        function = _source_function_at_line(text, 9235)
        leaves = _copy_and_blit_simple_image_leaf_names(function)
        self.assertTrue({path.rsplit(".", 1)[1] for path in required} <= leaves)
        self.assertIn("partial_image_pot_diff_format_clear", leaves)
        degraded = _copy_and_blit_simple_image_leaf_names(
            function.replace("+ clear.name", '+ "clear"', 1))
        self.assertNotIn("partial_image_pot_same_format_clear", degraded)
        self.assertFalse(_copy_and_blit_simple_image_leaf_names(
            function.replace("group->addChild(new CopyImageToImageTestCase"
                             "(testCtx, testCaseName, params));",
                             "group->addChild(makeCase(testCtx, testCaseName));", 1)))

    def test_fill_update_dynamic_names_are_derived_fail_closed(self):
        source = r'''
        tcu::TestCaseGroup *createFillAndUpdateBufferTests()
        {
            const std::string testName("buffer_second_part");
            group->addChild(new FillBufferTestCase(testCtx, "fill_" + testName, params));
            group->addChild(new UpdateBufferTestCase(testCtx, "update_" + testName, params));
            for (VkDeviceSize i = 0; i < sizeof(uint32_t); ++i)
            for (VkDeviceSize j = 0; j < sizeof(uint32_t); ++j) {
                params.dstOffset = j * sizeof(uint32_t);
                const std::string name = "fill_buffer_vk_whole_size_" + de::toString(extraBytes) +
                    "_extra_bytes_offset_" + de::toString(params.dstOffset);
            }
        }
        '''
        leaves = _fill_update_generated_leaf_names(source)
        self.assertIn("fill_buffer_second_part", leaves)
        self.assertIn("update_buffer_second_part", leaves)
        self.assertIn("fill_buffer_vk_whole_size_3_extra_bytes_offset_12", leaves)
        self.assertNotIn("update_buffer_vk_whole_size_3_extra_bytes_offset_12", leaves)
        degraded = _fill_update_generated_leaf_names(
            source.replace('"fill_" + testName', 'makeName(testName)', 1))
        self.assertNotIn("fill_buffer_second_part", degraded)
        self.assertNotIn("update_buffer_second_part", degraded)
        self.assertIn("fill_buffer_vk_whole_size_3_extra_bytes_offset_12", degraded)

    def test_buffer_copy_pruning_changes_registration_only(self):
        upstream = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/"
                    "modules/vulkan/api/vktApiCopiesAndBlittingTests.cpp")
        with tempfile.TemporaryDirectory() as directory:
            if not upstream.is_file():
                upstream = Path(directory) / "upstream.cpp"
                upstream.write_text(
                    """
void addCoreCopiesAndBlittingTests(tcu::TestCaseGroup *group)
{
    uint32_t extensionFlags = 0;
    addCopiesAndBlittingTests(group, ALLOCATION_KIND_SUBALLOCATED, extensionFlags);
    addBufferCopyOffsetTests(group);
}
void addImageToImageTestsSimpleOnly(tcu::TestCaseGroup *group, TestGroupParamsPtr testGroupParams)
{
    addTestGroup(group, "simple_tests", addImageToImageSimpleTests, testGroupParams);
}
void factory()
{
    copiesAndBlittingTests->addChild(createTestGroup(testCtx, "core", addCoreCopiesAndBlittingTests, cleanupGroup));
    copiesAndBlittingTests->addChild(
        createTestGroup(testCtx, "dedicated_allocation", addDedicatedAllocationCopiesAndBlittingTests, cleanupGroup));
    copiesAndBlittingTests->addChild(createTestGroup(
        testCtx, "copy_commands2",
        [](tcu::TestCaseGroup *group) { addCopiesAndBlittingTests(group, ALLOCATION_KIND_DEDICATED, COPY_COMMANDS_2); },
        cleanupGroup));
    copiesAndBlittingTests->addChild(createTestGroup(
        testCtx, "sparse",
        [](tcu::TestCaseGroup *group)
        { addSparseCopyTests(group, ALLOCATION_KIND_DEDICATED, COPY_COMMANDS_2 | SPARSE_BINDING); },
        cleanupGroup));
}
void CopyBufferToBuffer::iterate() {}
void oracle() { deMemCmp(referenceData, resultData, bufferSize); }
""",
                    encoding="utf-8",
                )
            destination = Path(directory) / "focused.cpp"
            write_focused_buffer_copy_source(upstream, destination)
            generated = destination.read_text()
        self.assertIn('addTestGroup(group, "buffer_to_buffer",', generated)
        self.assertIn('addTestGroup(group, "image_to_image",'
                      ' addImageToImageTestsSimpleOnly,', generated)
        self.assertIn('group, "simple_tests", addImageToImageSimpleTests', generated)
        self.assertNotIn('createTestGroup(testCtx, "dedicated_allocation"', generated)
        self.assertIn("CopyBufferToBuffer::iterate", generated)
        self.assertIn("deMemCmp(referenceData, resultData, bufferSize)", generated)

    def test_bc_blit_registration_preserves_original_implementations(self):
        upstream = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/"
                    "modules/vulkan/api/vktApiCopiesAndBlittingTests.cpp")
        if not upstream.is_file():
            self.skipTest("pinned CTS checkout unavailable")
        original = upstream.read_text()
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "focused.cpp"
            write_focused_buffer_copy_source(upstream, destination, include_bc_blits=True)
            generated = destination.read_text()
        marker = "void addBlittingImageAllFormatsColorSrcFormatTests("
        self.assertEqual(original[:original.index(marker)], generated[:generated.index(marker)])
        self.assertIn('addTestGroup(group, "blit_image", addBlittingImageTests,', generated)
        self.assertIn("srcFormat < VK_FORMAT_BC1_RGB_UNORM_BLOCK", generated)
        self.assertIn("srcFormat > VK_FORMAT_BC7_SRGB_BLOCK", generated)
        color = generated.split("void addBlittingImageAllFormatsColorTests(", 1)[1]
        color = color.split("void addBlittingImageAllFormatsDepthStencilFormatsTests(", 1)[0]
        self.assertIn("create2DCopyRegions(64, 64, 64, 64)", color)
        self.assertNotIn("// 1D tests.", color)
        self.assertNotIn("// 3D tests.", color)

    def test_bc_mip_copy_registration_preserves_original_implementations(self):
        upstream = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/"
                    "modules/vulkan/api/vktApiCopiesAndBlittingTests.cpp")
        if not upstream.is_file():
            self.skipTest("pinned CTS checkout unavailable")
        original = upstream.read_text()
        marker = "void add2dImageToBufferTests("
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "focused.cpp"
            for blits in (False, True):
                write_focused_buffer_copy_source(upstream, destination,
                    include_bc_blits=blits, include_bc_mip_copies=True)
                generated = destination.read_text()
                self.assertEqual(original[:original.index(marker)],
                                 generated[:generated.index(marker)])
                self.assertIn('addTestGroup(group, "image_to_buffer", addImageToBufferTests,', generated)
                registration = generated.split(marker, 1)[1].split(
                    "void addBufferToDepthStencilTests(", 1)[0]
                self.assertIn("{64, 64, 1}", registration)
                self.assertIn("{64, 192, 1}", registration)
                self.assertIn("arrayLayers[] = {1, 2, 5}", registration)
                self.assertIn("*format < VK_FORMAT_BC1_RGB_UNORM_BLOCK", registration)
                self.assertIn("*format > VK_FORMAT_BC7_SRGB_BLOCK", registration)
                self.assertEqual(registration.count("new CopyCompressedImageToBufferTestCase("), 1)
                self.assertIn('numLayers, "universal"), params)', registration)
                self.assertNotIn("QueueSelectionOptions::ComputeOnly", registration)
                self.assertNotIn("QueueSelectionOptions::TransferOnly", registration)
                self.assertNotIn("new CopyImageToBufferTestCase(", registration)
                core = generated.split("void addCoreCopiesAndBlittingTests(", 1)[1].split("\n}", 1)[0]
                self.assertEqual(blits, 'addTestGroup(group, "blit_image", addBlittingImageTests,' in core)
            drifted = Path(directory) / "drifted.cpp"
            drifted.write_text(original.replace("    // those tests are performed for all queues, no need to repeat them", "    // moved registration"))
            with self.assertRaisesRegex(SystemExit, "BC mip copy registration drift"):
                write_focused_buffer_copy_source(drifted, destination, include_bc_mip_copies=True)

    def test_robust_buffer_selection_and_upstream_factory_are_exact(self):
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        selected = {case["path"] for case in manifest["cases"]
                    if ".robustness.buffer_access." in case["path"]}
        self.assertEqual(self.ROBUST_BUFFER_CASES, selected)
        for case in manifest["cases"]:
            if case["path"] in selected:
                self.assertEqual(["robustBufferAccess"], case["features_required"])
                self.assertEqual("robust-buffer-access", case["category"])

        build = (REPO_ROOT / "tools/build_upstream_cts.py").read_text(encoding="utf-8")
        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text(encoding="utf-8")
        self.assertIn("write_focused_robust_buffer_source", build)
        self.assertIn("vktRobustnessUtil.cpp", build)
        self.assertIn("createBufferAccessTests", package)

    def test_robust_buffer_pruning_changes_registration_only_and_fails_closed(self):
        upstream = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/"
                    "modules/vulkan/robustness/vktRobustnessBufferAccessTests.cpp")
        if not upstream.is_file():
            self.skipTest("pinned upstream CTS checkout is not present")
        original = upstream.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "focused.cpp"
            write_focused_robust_buffer_source(upstream, destination)
            generated = destination.read_text(encoding="utf-8")
            self.assertIn("stage != VK_SHADER_STAGE_COMPUTE_BIT", generated)
            self.assertIn("shaderTypeNdx != SHADER_TYPE_SCALAR_COPY", generated)
            self.assertIn("bufferFormat != VK_FORMAT_R32_UINT", generated)
            # Representative shader and oracle text is not replaced.
            for needle in ("RobustBufferReadTest::initPrograms",
                           "RobustBufferWriteTest::createInstance"):
                self.assertEqual(original.count(needle), generated.count(needle))

            drifted = Path(directory) / "drifted.cpp"
            drifted.write_text(original.replace(
                "const VkShaderStageFlagBits stage = bufferAccessStages[stageNdx];",
                "const VkShaderStageFlagBits chosenStage = bufferAccessStages[stageNdx];",
                1), encoding="utf-8")
            with self.assertRaises(SystemExit):
                write_focused_robust_buffer_source(drifted, destination)

    def test_native_platform_supports_concurrent_logical_device_sessions(self):
        source = (REPO_ROOT / "native/platform_ps5.c").read_text(encoding="utf-8")
        self.assertNotIn("if (opened) return VK_ERROR_INITIALIZATION_FAILED", source)
        self.assertIn("PS5VK_PLATFORM_JOIN users=%u", source)
        self.assertIn("PS5VK_PLATFORM_LEAVE users=%u", source)
        self.assertIn("if (opened > 1)", source)
        self.assertIn("atomic_flag_test_and_set_explicit", source)
        self.assertLess(source.index("if (opened > 1)"),
                        source.index("if (budget.used)"))

    def test_indirect_dispatch_upstream_factory_body_and_selection_are_pinned(self):
        """Keep the two original indirect compute oracles linked and selected."""
        expected = {
            "dEQP-VK.compute.indirect_dispatch.upload_buffer.single_invocation",
            "dEQP-VK.compute.indirect_dispatch.gen_in_compute.single_invocation",
        }
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        selected = {case["path"] for case in manifest["cases"]}
        self.assertTrue(expected <= selected)

        build = (REPO_ROOT / "tools/build_upstream_cts.py").read_text(encoding="utf-8")
        package = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text(encoding="utf-8")
        self.assertIn("vktComputeIndirectComputeDispatchTests.cpp", build)
        self.assertIn("createIndirectComputeDispatchTests", package)

        upstream = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/"
                    "modules/vulkan/compute/vktComputeIndirectComputeDispatchTests.cpp")
        if upstream.is_file():
            body = upstream.read_text(encoding="utf-8")
            self.assertIn("cmdDispatchIndirect", body)
            self.assertIn('"gen_in_compute"', body)
            self.assertIn('"upload_buffer"', body)

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
        # A payload may legitimately be packaged from a measurement selection
        # (tools/make_measurement_manifest.py); the build manifest records that
        # derivation. The invariant is the same in both cases - the packaged
        # list and hash derive from the manifest the build was given - so read
        # the selection the build actually used instead of assuming the frozen
        # one, and keep the frozen expectation for an ordinary build.
        build_manifest_path = dist / "build_manifest.json"
        if build_manifest_path.is_file():
            build_manifest = json.loads(build_manifest_path.read_text())
            measurement = build_manifest.get("measurement")
            if measurement:
                self.assertEqual(expected_hash, measurement["base_selection_hash"],
                                 "the measurement payload was derived from a different "
                                 "frozen selection than the one in the tree")
                expected = "\n".join(build_manifest["selected_cases"]) + "\n"
                expected_hash = hashlib.sha256(expected.encode("utf-8")).hexdigest()
                self.assertEqual(expected_hash, measurement["selection_hash"])
        self.assertEqual((dist / "cases.txt").read_text(encoding="utf-8"), expected)
        self.assertEqual((dist / "selection_hash.txt").read_text(encoding="utf-8").strip(),
                         expected_hash)


class TestSamplerBindingModelPromotion(unittest.TestCase):
    """The three promoted original binding-model combined-sampler cases.

    These leaves became strict acceptance only after their unchanged upstream
    oracles measured `Pass` twice on one signed artifact.  The promotion is a
    registration and selection change: no Khronos test body, shader, support
    check, reference image or result oracle is modified.  The tests below keep
    the promotion from being dropped, renamed, duplicated or replaced, and keep
    the strict verifier honest if any of those four edits is made.
    """

    PROMOTED = {
        "dEQP-VK.binding_model.shader_access.primary_cmd_buf.bind."
        "combined_image_sampler_mutable.vertex.single_descriptor.2d",
        "dEQP-VK.binding_model.shader_access.primary_cmd_buf.bind."
        "combined_image_sampler_mutable.fragment.single_descriptor.2d",
        "dEQP-VK.binding_model.shader_access.primary_cmd_buf.bind."
        "combined_image_sampler_mutable.vertex_fragment.single_descriptor.2d",
    }
    PROMOTED_SOURCE = ("external/vulkancts/modules/vulkan/binding_model/"
                       "vktBindingShaderAccessTests.cpp:9698")
    UPSTREAM_SOURCE = (REPO_ROOT / "third_party/vk-gl-cts/external/vulkancts/"
                       "modules/vulkan/binding_model/vktBindingShaderAccessTests.cpp")

    @staticmethod
    def _qpa(paths):
        blocks = "".join(
            f"#beginTestCaseResult {path}\n"
            f'<TestCaseResult CasePath="{path}">\n'
            '  <Result StatusCode="Pass">Pass</Result>\n'
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            for path in paths
        )
        return ("#sessionInfo releaseName 1.3.8.4\n#beginSession\n"
                + blocks + "#endSession\n")

    @staticmethod
    def _bounded_manifest(paths):
        return {"cases": [{"path": path} for path in sorted(paths)]}

    @staticmethod
    def _run_selection_gate(manifest):
        """Run the frozen-selection gate against a candidate manifest."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(manifest, indent=1), encoding="utf-8")
            stdout, stderr = io.StringIO(), io.StringIO()
            with mock.patch.object(upstream_selection, "MANIFEST", path), \
                    contextlib.redirect_stdout(stdout), \
                    contextlib.redirect_stderr(stderr):
                code = upstream_selection.main()
            return code, stdout.getvalue() + stderr.getvalue()

    def test_promoted_cases_are_strict_acceptance_and_original(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        accepted = {case["path"]: case for case in manifest["cases"]}
        diagnostics = {case["path"] for case in manifest.get("diagnostics", [])}
        self.assertEqual(self.PROMOTED, self.PROMOTED & set(accepted))
        self.assertFalse(self.PROMOTED & diagnostics)
        for path in sorted(self.PROMOTED):
            case = accepted[path]
            self.assertEqual("graphics-sampler", case["category"], path)
            self.assertEqual("Pass", case["expected_status"], path)
            self.assertEqual([], case["features_required"], path)
            self.assertEqual(self.PROMOTED_SOURCE, case["source"], path)
            self.assertTrue(case["rationale"], path)

        # The integration registers the upstream access-test family wholesale
        # and the build compiles its module, so selection alone promotes them.
        integration = (REPO_ROOT / "cts/upstream/package_ps5.cpp").read_text(
            encoding="utf-8")
        builder = (REPO_ROOT / "tools/build_upstream_cts.py").read_text(
            encoding="utf-8")
        self.assertIn("createShaderAccessTests", integration)
        self.assertIn("vktBindingShaderAccessTests.cpp", builder)

        if not self.UPSTREAM_SOURCE.is_file():
            return
        text = self.UPSTREAM_SOURCE.read_text(encoding="utf-8")
        function = _source_function_at_line(text, 9698)
        self.assertIn("createShaderAccessTests", function)
        for leaf in ("combined_image_sampler_mutable", "single_descriptor"):
            self.assertIn(f'"{leaf}"', text)
        for stage in ("vertex", "fragment", "vertex_fragment"):
            self.assertIn(f'"{stage}"', text)

    def test_selection_gate_rejects_a_duplicated_promoted_case(self):
        """A repeated entry is not a larger selection and must fail closed."""
        manifest = json.loads(MANIFEST_PATH.read_text())
        duplicated = copy.deepcopy(manifest)
        duplicated["cases"].append(copy.deepcopy(
            next(case for case in duplicated["cases"]
                 if case["path"] in self.PROMOTED)))
        code, output = self._run_selection_gate(duplicated)
        self.assertEqual(1, code, output)
        self.assertIn("selected twice", output)
        # The same leaf cannot be both acceptance and diagnostic either.
        conflicted = copy.deepcopy(manifest)
        conflicted["diagnostics"].append(copy.deepcopy(
            next(case for case in conflicted["cases"]
                 if case["path"] in self.PROMOTED)))
        code, output = self._run_selection_gate(conflicted)
        self.assertEqual(1, code, output)
        self.assertIn("selected twice", output)

    def test_selection_gate_rejects_a_renamed_promoted_case(self):
        """An invented leaf must not be traceable to the pinned factory."""
        if not upstream_selection.UPSTREAM.is_dir():
            self.skipTest("pinned vk-gl-cts checkout not present")
        manifest = json.loads(MANIFEST_PATH.read_text())
        for suffix in ("vertex", "fragment", "vertex_fragment"):
            renamed = copy.deepcopy(manifest)
            original = ("dEQP-VK.binding_model.shader_access.primary_cmd_buf."
                        "bind.combined_image_sampler_mutable."
                        f"{suffix}.single_descriptor.2d")
            replacement = original.replace(
                "single_descriptor.2d", "single_descriptor.2d_invented")
            for case in renamed["cases"]:
                if case["path"] == original:
                    case["path"] = replacement
            code, output = self._run_selection_gate(renamed)
            self.assertEqual(1, code, output)
            self.assertIn("is not registered in", output)

    def test_selection_gate_rejects_a_replaced_promoted_case(self):
        """A replacement must still name a group the pinned sources produce."""
        if not upstream_selection.UPSTREAM.is_dir():
            self.skipTest("pinned vk-gl-cts checkout not present")
        manifest = json.loads(MANIFEST_PATH.read_text())
        for suffix in ("vertex", "fragment", "vertex_fragment"):
            replaced = copy.deepcopy(manifest)
            original = ("dEQP-VK.binding_model.shader_access.primary_cmd_buf."
                        "bind.combined_image_sampler_mutable."
                        f"{suffix}.single_descriptor.2d")
            invented = ("dEQP-VK.binding_model.shader_access.primary_cmd_buf."
                        "bind.combined_image_sampler_invented."
                        f"{suffix}.single_descriptor.2d")
            for case in replaced["cases"]:
                if case["path"] == original:
                    case["path"] = invented
            code, output = self._run_selection_gate(replaced)
            self.assertEqual(1, code, output)
            self.assertIn("combined_image_sampler_invented", output)

    def test_strict_acceptance_rejects_dropped_renamed_or_duplicated_results(self):
        """The verifier must reject the same four edits on the measurement side."""
        bounded = self._bounded_manifest(self.PROMOTED)
        meta = {"exit_code": 0}

        complete = parse_qpa_results(self._qpa(sorted(self.PROMOTED)))
        verified = verify_upstream_acceptance(bounded, meta, complete)
        self.assertTrue(verified["strict_verified"])
        self.assertEqual(3, verified["pass_count"])

        # Dropping one promotion from the expected set makes its result extra.
        dropped_path = sorted(self.PROMOTED)[0]
        dropped = self._bounded_manifest(self.PROMOTED - {dropped_path})
        verified = verify_upstream_acceptance(dropped, meta, complete)
        self.assertFalse(verified["strict_verified"])
        self.assertEqual([dropped_path], verified["unexpected"])
        self.assertEqual([], verified["missing"])

        # Renaming the reported case loses the expected leaf and adds a stranger.
        renamed_path = dropped_path + "_renamed"
        renamed_results = parse_qpa_results(self._qpa(
            sorted((self.PROMOTED - {dropped_path}) | {renamed_path})))
        verified = verify_upstream_acceptance(bounded, meta, renamed_results)
        self.assertFalse(verified["strict_verified"])
        self.assertEqual([dropped_path], verified["missing"])
        self.assertEqual([renamed_path], verified["unexpected"])

        # Reporting the same leaf twice is not two passing cases.
        duplicated_results = parse_qpa_results(self._qpa(
            sorted(self.PROMOTED) + [dropped_path]))
        verified = verify_upstream_acceptance(bounded, meta, duplicated_results)
        self.assertFalse(verified["strict_verified"])
        self.assertEqual([dropped_path], verified["duplicates"])
        self.assertEqual(4, verified["total_reported"])


if __name__ == "__main__":
    unittest.main()

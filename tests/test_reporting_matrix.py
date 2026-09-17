import json
import hashlib
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import check_reporting_matrix as matrix  # noqa: E402

# Scope guard for the Vulkan 1.0 4-byte/32-bit format-contract task: the rows
# of every other mandatory format table are frozen by digest, so a change that
# should have stayed inside the assigned tables cannot pass unnoticed. A
# deliberate change to another table must update this digest in the same commit
# that changes the table, with the reason in the commit message.
UNASSIGNED_FORMAT_TABLES = {
    "formats-mandatory-features-subbyte",
    "formats-mandatory-features-2byte",
    "formats-mandatory-features-10bit",
    "formats-mandatory-features-16bit",
    "formats-mandatory-features-64bit",
    "formats-mandatory-features-depth-stencil",
    "formats-mandatory-features-bcn",
    "formats-mandatory-features-etc",
    "formats-mandatory-features-astc",
}
UNASSIGNED_FORMAT_TABLE_DIGEST = (
    # The 2026-09-15 direct UTEXEL matrix intentionally qualifies owned rows
    # across the 2-byte, 4-byte, 16-bit, 32-bit and 64-bit tables.
    "529b68fbd880f0cf9a4a49cbe3631302790217100c569ee658288fb9864b69a7")


def unassigned_format_table_digest(rows):
    blob = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(blob).hexdigest()


class TestReportingMatrix(unittest.TestCase):
    def test_multiview_queries_are_graphics_only_and_keep_the_route(self):
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        self.assertEqual(
            {"route": "VK_KHR_multiview", "multiview": True,
             "maxMultiviewViewCount": 6, "maxMultiviewInstanceIndex": 134217727},
            data["profiles"]["graphics"]["multiview_query"])
        compute = data["profiles"]["compute"]["multiview_query"]
        self.assertFalse(compute["multiview"])
        self.assertEqual(0, compute["maxMultiviewViewCount"])
        self.assertEqual(0, compute["maxMultiviewInstanceIndex"])

    def test_cross_table_format_snapshot_matches_the_audited_baseline(self):
        """Changes outside the original two-table audit require explicit review."""
        data = json.loads(
            (ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        rows = [row for row in data["formats"]
                if row.get("table") in UNASSIGNED_FORMAT_TABLES]
        self.assertTrue(rows)
        self.assertEqual(unassigned_format_table_digest(rows),
                         UNASSIGNED_FORMAT_TABLE_DIGEST,
                         "a cross-table format row changed; if deliberate, update "
                         "UNASSIGNED_FORMAT_TABLE_DIGEST and explain why")

    def test_every_public_format_is_in_the_reporting_dump(self):
        """A supported image or vertex row omitted by the dumper is a false blocker."""
        public_formats = set()
        for path in (ROOT / "src/texture_format.c", ROOT / "src/graphics_formats.h"):
            public_formats |= set(re.findall(r"\bVK_FORMAT_[A-Z0-9_]+\b",
                                             path.read_text()))
        public_formats = {name for name in public_formats
                          if not name.startswith("VK_FORMAT_FEATURE_")}
        dump_source = (ROOT / "tools/dump_device_reporting.c").read_text()
        dump_body = dump_source.split("static const VkFormat dump_formats[] = {", 1)[1]
        dump_body = dump_body.split("};", 1)[0]
        dumped_formats = set(re.findall(r"\bVK_FORMAT_[A-Z0-9_]+\b", dump_body))
        self.assertEqual(public_formats - dumped_formats, set())

    def test_committed_matrix_is_current(self):
        """The gate must fail when the reported values move without a refresh."""
        # `make check` builds this dump in its own step and runs the same check;
        # when only the unit suite is run, skip rather than build a second copy.
        if not (ROOT / "build/tests/dump_device_reporting").is_file():
            self.skipTest("reporting dump not built; `make check` builds and runs it")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/check_reporting_matrix.py"), "--check"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sampled_descriptor_floors_are_reported_and_still_bound(self):
        """The four sampled-descriptor floors are satisfied in graphics only."""
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        rows = {(row["profile"], row["limit"]): row for row in data["limits"]}
        for name, value in (("maxPerStageDescriptorSamplers", 16),
                            ("maxPerStageDescriptorSampledImages", 16),
                            ("maxDescriptorSetSamplers", 96),
                            ("maxDescriptorSetSampledImages", 96)):
            row = rows[("graphics", name)]
            self.assertEqual(row["reported"], value, name)
            self.assertEqual(row["verdict"], "satisfied", name)
            # The compute-only build applies no graphics limits and stays a
            # documented blocker rather than a claim.
            compute = rows[("compute", name)]
            self.assertEqual(compute["verdict"], "blocker", name)
            self.assertIn("compute-only build", compute["detail"], name)
        # A dropped value is still a violation unless it is a documented blocker.
        core = json.loads((ROOT / "conformance_inventory/core_target.json").read_text())
        sampler_row = next(r for r in core["limits"]["rows"]
                           if r["limit"] == "maxPerStageDescriptorSamplers")
        verdict, _ = matrix.evaluate_limit(sampler_row, 15, {})
        self.assertEqual(verdict, "violation")
        sampler_set_row = next(r for r in core["limits"]["rows"]
                               if r["limit"] == "maxDescriptorSetSamplers")
        verdict, _ = matrix.evaluate_limit(sampler_set_row, 95, {})
        self.assertEqual(verdict, "violation")

    def test_manifest_report_mismatch_fails_the_gate(self):
        """A selection the committed matrix does not record is a gate failure."""
        manifest = json.loads((ROOT / "cts/upstream/manifest.json").read_text())
        manifest["cases"] = manifest["cases"][:-1]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(manifest))
            original, argv = matrix.MANIFEST, sys.argv
            matrix.MANIFEST, sys.argv = path, ["check_reporting_matrix.py", "--check"]
            try:
                self.assertEqual(matrix.main(), 1)
            finally:
                matrix.MANIFEST, sys.argv = original, argv

    def test_below_floor_report_without_a_blocker_is_a_violation(self):
        """A new below-floor value must fail the gate, not be silently accepted."""
        core = json.loads((ROOT / "conformance_inventory/core_target.json").read_text())
        row = next(r for r in core["limits"]["rows"]
                   if r["limit"] == "maxPushConstantsSize")
        verdict, _ = matrix.evaluate_limit(row, 128, {})
        self.assertEqual(verdict, "satisfied")
        verdict, _ = matrix.evaluate_limit(row, 127, {})
        self.assertEqual(verdict, "violation")
        self.assertNotIn("maxPushConstantsSize", matrix.KNOWN_BLOCKERS)
        self.assertNotIn("maxDescriptorSetUniformBuffersDynamic", matrix.KNOWN_BLOCKERS)
        self.assertNotIn("maxDescriptorSetStorageBuffersDynamic", matrix.KNOWN_BLOCKERS)
        # Documented blockers are the accepted way to stay below the floor; the
        # mapping happens once, after evaluation, and an unlisted limit would
        # keep its "violation" verdict and fail the gate.
        array_row = next(r for r in core["limits"]["rows"]
                         if r["limit"] == "maxImageArrayLayers")
        verdict, _ = matrix.evaluate_limit(array_row, 1, {})
        self.assertEqual(verdict, "violation")
        committed = json.loads(
            (ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        array_rows = {r["profile"]: r for r in committed["limits"]
                      if r["limit"] == "maxImageArrayLayers"}
        self.assertEqual(array_rows["graphics"]["verdict"], "satisfied")
        self.assertEqual(array_rows["compute"]["verdict"], "blocker")
        self.assertIn("maxColorAttachments", matrix.KNOWN_BLOCKERS)

    def test_every_blocker_still_describes_a_real_below_floor_report(self):
        """A blocker entry cannot be used to excuse a value that already passes."""
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        blocked = {row["limit"] for row in data["limits"] if row["verdict"] == "blocker"}
        self.assertTrue(blocked)
        self.assertTrue(blocked <= set(matrix.KNOWN_BLOCKERS))

    def test_feature_gate_citations_resolve(self):
        """Feature rows must cite a code path that is still present."""
        for name, (where, token, _reason) in matrix.FEATURE_GATES.items():
            text = (ROOT / where).read_text()
            self.assertIn(token, text, f"{name} cites a missing token in {where}")
        gate_file, gate_token, test_file, test_token = matrix.FALSE_CORE_FEATURE_GATE
        self.assertIn(gate_token, (ROOT / gate_file).read_text())
        self.assertIn(test_token, (ROOT / test_file).read_text())

    def test_every_false_core_feature_has_a_fail_closed_negotiation_gate(self):
        """A compiler-side feature needs no invented object gate, but it must not enable."""
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        false_rows = [row for row in data["features"] if row["reported"] is False]
        self.assertTrue(false_rows)
        for row in false_rows:
            verdict, detail = matrix.evaluate_feature(row["feature"], False, row["profile"])
            self.assertEqual(verdict, "satisfied", row["feature"])
            self.assertTrue(detail)
        self.assertFalse([row for row in data["features"]
                          if row["verdict"] == "not-audited"])

    def test_advertised_features_are_explicit_and_fail_closed(self):
        """A true bit needs reviewed code evidence and a selected oracle."""
        verdict, _ = matrix.evaluate_feature("robustBufferAccess", True)
        self.assertEqual(verdict, "satisfied")
        verdict, _ = matrix.evaluate_feature("robustBufferAccess", False)
        self.assertEqual(verdict, "violation")
        verdict, _ = matrix.evaluate_feature("shaderInt64", True)
        self.assertEqual(verdict, "violation")
        entry = matrix.ADVERTISED_FEATURES["robustBufferAccess"]
        self.assertEqual(entry["cts"], ("dEQP-VK.info.device_mandatory_features",))
        for where, token in entry["citations"]:
            self.assertIn(token, (ROOT / where).read_text())

    def test_shader_capability_gate_is_mapped(self):
        """Every capability the frontend handles is classified."""
        source = (ROOT / "src/vk_pipeline.c").read_text()
        self.assertIn("spirv_narrow_requirements", source)
        self.assertIn(22, matrix.SHADER_CAPABILITY_ADVERTISEMENT)
        self.assertIn(4433, matrix.SHADER_CAPABILITY_ADVERTISEMENT)

    def test_hardware_validated_linear_filter_claim_is_exactly_scoped(self):
        """Only mandatory formats covered by the executable filter table become green."""
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        rows = [row for row in data["formats"]
                if row.get("feature") == "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT"]
        satisfied = {row["format"] for row in rows
                     if row.get("profile") == "graphics" and
                     row.get("verdict") == "satisfied"}
        self.assertEqual(satisfied, {
            "VK_FORMAT_R8_UNORM", "VK_FORMAT_R8_SNORM",
            "VK_FORMAT_R8G8_UNORM", "VK_FORMAT_R8G8_SNORM",
            "VK_FORMAT_R8G8B8A8_UNORM", "VK_FORMAT_R8G8B8A8_SNORM",
            "VK_FORMAT_R8G8B8A8_SRGB",
            "VK_FORMAT_R16_SFLOAT", "VK_FORMAT_R16G16_SFLOAT",
            "VK_FORMAT_R16G16B16A16_SFLOAT",
            "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32",
            "VK_FORMAT_B10G11R11_UFLOAT_PACK32",
            "VK_FORMAT_A8B8G8R8_UNORM_PACK32",
            "VK_FORMAT_A8B8G8R8_SNORM_PACK32",
            "VK_FORMAT_A8B8G8R8_SRGB_PACK32",
        })
        self.assertTrue(any(row.get("verdict") == "blocker" and
                            row.get("format") != "VK_FORMAT_R8G8B8A8_UNORM"
                            for row in rows))

    def test_integer_sampling_is_reported_without_linear_filtering(self):
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        integer = {
            f"VK_FORMAT_R{width}{suffix}_{sign}"
            for width in (8, 16, 32)
            for suffix in ("", f"G{width}", f"G{width}B{width}A{width}")
            for sign in ("UINT", "SINT")
        }
        integer |= {"VK_FORMAT_A8B8G8R8_UINT_PACK32", "VK_FORMAT_A8B8G8R8_SINT_PACK32"}
        sampled = {row["format"] for row in data["formats"]
                   if row.get("profile") == "graphics" and
                   row.get("feature") == "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT" and
                   row.get("verdict") == "satisfied"}
        linear = {row["format"] for row in data["formats"]
                  if row.get("profile") == "graphics" and
                  row.get("feature") == "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT" and
                  row.get("verdict") == "satisfied"}
        self.assertTrue(integer <= sampled)
        self.assertTrue(integer.isdisjoint(linear))
        integer_limits = [row for row in data["limits"]
                          if row.get("limit") == "sampledImageIntegerSampleCounts"]
        self.assertEqual({row["profile"] for row in integer_limits}, {"compute", "graphics"})
        self.assertTrue(all(row["verdict"] == "satisfied" for row in integer_limits))

    def test_documented_format_counts_follow_the_generated_matrix(self):
        """Narrative summaries must not retain stale format totals."""
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        counts = data["summary"]["formats"]
        phrase = (f'{counts["satisfied"]} mandatory format-feature cells satisfied '
                  f'and {counts["blocker"]} per-format blockers recorded')
        requirements = json.loads(
            (ROOT / "conformance_inventory/requirements.json").read_text())
        row = next(item for item in requirements["requirements"]
                   if item["id"] == "VK14-FORMATS-001")
        self.assertIn(phrase, row["cts"]["note"])
        validation = (ROOT / "VALIDATION.md").read_text()
        self.assertIn(
            f'{counts["satisfied"]} mandatory format-feature cells satisfied\n'
            f'with {counts["blocker"]} documented per-format blockers',
            validation)


if __name__ == "__main__":
    unittest.main()

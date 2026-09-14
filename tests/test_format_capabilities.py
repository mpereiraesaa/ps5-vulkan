"""Audit the authoritative format-capability contract.

The driver publishes a format feature bit only for a capability that has an
on-console witness. These tests close the loop between the three artefacts that
must agree: the capability ledger emitted by ``tools/dump_device_reporting.c``,
the committed ``conformance_inventory/reporting_matrix.json`` and
``conformance_inventory/physical_format_validation.json``.

A capability advertised without a backend, or a promotion list that names a
format the table does not implement, fails here.
"""

import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import check_reporting_matrix as matrix  # noqa: E402

DUMP = ROOT / "build/tests/dump_device_reporting"
PLAN = ROOT / "conformance_inventory/physical_format_validation.json"
MATRIX = ROOT / "conformance_inventory/reporting_matrix.json"

# The capability ABI, restated independently of the C header so a silent
# renumbering is caught.
CAP = {
    "TRANSFER_DST": 1 << 0,
    "TRANSFER_SRC": 1 << 1,
    "SAMPLED_IMAGE": 1 << 2,
    "SAMPLED_IMAGE_LINEAR": 1 << 3,
    "COLOR_ATTACHMENT": 1 << 4,
    "COLOR_ATTACHMENT_READBACK": 1 << 5,
    "COLOR_ATTACHMENT_BLEND": 1 << 6,
    "DEPTH_STENCIL_ATTACHMENT": 1 << 7,
    "VERTEX_BUFFER": 1 << 8,
    "UNIFORM_TEXEL_BUFFER": 1 << 9,
    "STORAGE_TEXEL_BUFFER": 1 << 10,
    "STORAGE_IMAGE": 1 << 11,
    "STORAGE_IMAGE_ATOMIC": 1 << 12,
    "BLIT_SRC": 1 << 13,
    "BLIT_DST": 1 << 14,
}
OPTIMAL_FEATURES = {
    "TRANSFER_DST": "VK_FORMAT_FEATURE_TRANSFER_DST_BIT",
    "TRANSFER_SRC": "VK_FORMAT_FEATURE_TRANSFER_SRC_BIT",
    "SAMPLED_IMAGE": "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT",
    "SAMPLED_IMAGE_LINEAR": "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT",
    "COLOR_ATTACHMENT": "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT",
    "COLOR_ATTACHMENT_BLEND": "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT",
    "DEPTH_STENCIL_ATTACHMENT": "VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT",
    "STORAGE_IMAGE": "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT",
    "STORAGE_IMAGE_ATOMIC": "VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT",
    "BLIT_SRC": "VK_FORMAT_FEATURE_BLIT_SRC_BIT",
    "BLIT_DST": "VK_FORMAT_FEATURE_BLIT_DST_BIT",
}
BUFFER_FEATURES = {
    "VERTEX_BUFFER": "VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT",
    "UNIFORM_TEXEL_BUFFER": "VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT",
    "STORAGE_TEXEL_BUFFER": "VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT",
}
REGISTRY_PACKING = 1 << 1
TASK_TABLES = {
    "formats-mandatory-features-4byte",
    "formats-mandatory-features-32bit",
}
BLOCKER_REASON_FEATURES = {
    "no-blit-implementation": {
        "VK_FORMAT_FEATURE_BLIT_SRC_BIT",
        "VK_FORMAT_FEATURE_BLIT_DST_BIT",
    },
    "no-texel-buffer-descriptor": {
        "VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT",
        "VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT",
    },
    "no-render-target-encoding": {
        "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT",
        "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT",
    },
    "no-storage-image-abi": {
        "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT",
        "VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT",
    },
    "implemented-pending-physical-diagnostic": {
        "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT",
        "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT",
    },
    "no-sampled-encoding": {
        "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT",
        "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT",
    },
}
PENDING_FORMATS = {
    "VK_FORMAT_A8B8G8R8_UNORM_PACK32",
    "VK_FORMAT_A8B8G8R8_SNORM_PACK32",
    "VK_FORMAT_A8B8G8R8_SRGB_PACK32",
    "VK_FORMAT_A8B8G8R8_UINT_PACK32",
    "VK_FORMAT_A8B8G8R8_SINT_PACK32",
}
BGRA_FORMATS = {"VK_FORMAT_B8G8R8A8_UNORM", "VK_FORMAT_B8G8R8A8_SRGB"}


class TestFormatCapabilities(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not DUMP.is_file():
            raise unittest.SkipTest("reporting dump not built; `make check` builds it")
        result = subprocess.run([str(DUMP)], capture_output=True, text=True, check=True)
        cls.dump = json.loads(result.stdout)
        cls.ledger = cls.dump["formatCapabilities"]
        cls.matrix = json.loads(MATRIX.read_text())
        cls.plan = json.loads(PLAN.read_text())
        cls.names = {value: name for name, value in matrix.FORMAT_ENUMS.items()}
        cls.by_name = {cls.names[row["format"]]: row for row in cls.ledger}
        cls.format_rows = [row for row in cls.matrix["formats"]
                           if row.get("table") in TASK_TABLES]

    def test_capability_flags_are_a_superset_of_the_witnessed_set(self):
        for row in self.ledger:
            name = self.names[row["format"]]
            self.assertEqual(row["witnessed"] & ~row["capabilities"], 0, name)
            self.assertTrue(row["capabilities"], name)
            self.assertEqual(row["capabilities"] & ~sum(CAP.values()), 0, name)

    def test_published_features_are_exactly_the_witnessed_capabilities(self):
        """A bit may not be reported without a witnessed capability."""
        for row in self.ledger:
            name = self.names[row["format"]]
            optimal = 0
            buffered = 0
            for cap, flag in OPTIMAL_FEATURES.items():
                if row["witnessed"] & CAP[cap]:
                    optimal |= matrix.FEATURE_BITS[flag]
            for cap, flag in BUFFER_FEATURES.items():
                if row["witnessed"] & CAP[cap]:
                    buffered |= matrix.FEATURE_BITS[flag]
            self.assertEqual(row["optimalTilingFeatures"], optimal, name)
            self.assertEqual(row["bufferFeatures"], buffered, name)

    def test_implemented_sampled_capabilities_carry_a_complete_encoding(self):
        for row in self.ledger:
            name = self.names[row["format"]]
            if row["capabilities"] & (CAP["SAMPLED_IMAGE"] | CAP["SAMPLED_IMAGE_LINEAR"]):
                self.assertTrue(row["descriptorFormatWord"], name)
                self.assertTrue(row["bytesPerTexel"], name)
                self.assertEqual(len(row["selectors"]), 4, name)

    def test_linear_filtering_never_applies_to_integer_formats(self):
        for row in self.ledger:
            name = self.names[row["format"]]
            if not (row["capabilities"] & CAP["SAMPLED_IMAGE_LINEAR"]):
                continue
            self.assertNotIn("_UINT", name, name)
            self.assertNotIn("_SINT", name, name)

    def test_no_satisfied_cell_lacks_a_witnessed_capability(self):
        """The committed matrix may not report a feature the ledger cannot back."""
        satisfied = [row for row in self.format_rows
                     if row["verdict"] == "satisfied"
                     and row.get("feature", "").startswith("VK_FORMAT_FEATURE_")]
        self.assertTrue(satisfied)
        scope_map = {
            "optimalTilingFeatures": OPTIMAL_FEATURES,
            "bufferFeatures": BUFFER_FEATURES,
        }
        for row in satisfied:
            entry = self.by_name[row["format"]]
            cap = next(name for name, flag in scope_map[row["scope"]].items()
                       if flag == row["feature"])
            self.assertTrue(row["profile"] in ("graphics", "compute"))
            if row["profile"] != "graphics":
                continue
            self.assertTrue(entry["witnessed"] & CAP[cap], row["format"])

    def test_every_pending_entry_is_implemented_but_unwitnessed(self):
        entries = self.plan["entries"]
        self.assertEqual(len(entries), self.plan["summary"]["ready_pending_physical_validation"])
        for entry in entries:
            self.assertIn(entry["table"], TASK_TABLES)
            self.assertEqual(entry["profile"], "graphics")
            self.assertIn(entry["format"], PENDING_FORMATS)
            row = self.by_name[entry["format"]]
            cap = entry["capability"].replace("PS5VK_FORMAT_CAP_", "")
            self.assertTrue(row["capabilities"] & CAP[cap], entry["capability"])
            self.assertFalse(row["witnessed"] & CAP[cap], entry["capability"])
            for field in ("minimal_gpu_operation", "expected_result", "shader_type",
                          "host_test_limit", "pending_flag"):
                self.assertTrue(entry[field], (entry["format"], field))
            # The named feature must still be a blocker in the committed matrix.
            blockers = [cell for cell in self.format_rows
                        if cell["format"] == entry["format"]
                        and cell.get("feature") == entry["feature"]
                        and cell["verdict"] == "blocker"]
            self.assertTrue(blockers, (entry["format"], entry["feature"]))

    def test_pending_formats_keep_the_registry_packing_provenance(self):
        for name in PENDING_FORMATS:
            self.assertTrue(self.by_name[name]["provenance"] & REGISTRY_PACKING, name)

    def test_residual_blocker_classification_matches_the_matrix(self):
        classification = {row["reason"]: row for row in self.plan["blocker_classification"]}
        total = sum(row["cells"] for row in classification.values())
        summary = self.plan["summary"]
        self.assertEqual(total, summary["mandatory_cells_in_scope"])
        self.assertEqual(
            total - classification["implemented-pending-physical-diagnostic"]["cells"],
            summary["blocked_without_backend"])
        self.assertEqual(total,
                         summary["satisfied_by_this_task"]
                         + summary["ready_pending_physical_validation"]
                         + summary["blocked_without_backend"])
        blockers = [row for row in self.format_rows if row["verdict"] == "blocker"]
        self.assertEqual(len(blockers), self.plan["summary"]["mandatory_cells_in_scope"])
        by_reason = {}
        for name, row in classification.items():
            features = BLOCKER_REASON_FEATURES[name] if name != "profile-has-no-image-model" else None
            if name == "profile-has-no-image-model":
                matches = lambda r: r["profile"] == "compute"
            elif name == "implemented-pending-physical-diagnostic":
                matches = lambda r: (r["profile"] == "graphics"
                                     and r["format"] in PENDING_FORMATS
                                     and r["feature"] in features)
            elif name == "no-sampled-encoding":
                matches = lambda r: (r["profile"] == "graphics"
                                     and r["format"] in BGRA_FORMATS
                                     and r["feature"] in features)
            else:
                matches = lambda r: r["profile"] == "graphics" and r["feature"] in features
            by_reason[name] = sum(1 for row in blockers if matches(row))
        self.assertEqual(by_reason, {name: row["cells"] for name, row in classification.items()})
        # The reasons must partition the blocker set, not merely add up.
        self.assertEqual(sum(by_reason.values()), len(blockers))

    def test_the_task_tables_hold_no_unknown_state(self):
        allowed = {"satisfied", "blocker", "not-applicable"}
        self.assertTrue(all(row["verdict"] in allowed for row in self.format_rows))


if __name__ == "__main__":
    unittest.main()

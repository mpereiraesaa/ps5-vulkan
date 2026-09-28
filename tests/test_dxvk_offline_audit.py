"""Offline DXVK work queue must cover every blocker without promoting one."""
import copy
import hashlib
import json
import tempfile
import unittest

from pathlib import Path

from tools.audit_dxvk_offline import (
    ROOT, INLINE_PLAN, SIZE_MEASUREMENT, SIZE_PREVIOUS, audit_inline, build_audit,
)


class DxvkOfflineAuditTests(unittest.TestCase):
    def test_current_audit_covers_all_17_and_checks_the_inline_artifacts(self):
        if not all((ROOT / path).is_file() for path in
                   (INLINE_PLAN, SIZE_MEASUREMENT, SIZE_PREVIOUS)):
            self.skipTest("local offline candidate artifacts unavailable")
        report = build_audit()
        self.assertEqual(17, len(report["rows"]))
        self.assertEqual(17, len({r["id"] for r in report["rows"]}))
        self.assertEqual(8, report["inline_plan"]["checked_eboots"])
        self.assertTrue(report["inline_plan"]["execution_prepared"])
        self.assertTrue(report["subgroup_size_selection"]["selection_verified"])
        self.assertEqual(2, len(report["subgroup_size_selection"]["new_flagged_leaves"]))
        self.assertFalse(report["subgroup_size_selection"]["previous_candidate_covers_new_flags"])
        self.assertEqual(7, sum(r["phase"] == "native_validation" for r in report["rows"]))
        self.assertTrue(all(r["hardware_evidence_required"] for r in report["rows"]))

    def test_inline_audit_requires_all_eight_hashes_and_cts_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gates = []
            for index in range(8):
                name = "original_cts_limits" if index == 7 else f"gate_{index}"
                candidate = f"candidate-{index}"
                payload = f"payload-{index}".encode()
                target = root / candidate / "PPSA99994"
                target.mkdir(parents=True)
                (target / "eboot.bin").write_bytes(payload)
                gates.append({"gate": name, "candidate": candidate,
                              "eboot_sha256": hashlib.sha256(payload).hexdigest()})
            (root / "candidate-7" / "PPSA99994" / "selection_hash.txt").write_text("selected\n")
            gates[-1]["selection_hash"] = "selected"
            plan = {"order": gates, "native_executed": False}
            self.assertTrue(audit_inline(plan, root)["execution_prepared"])
            altered = copy.deepcopy(plan)
            altered["order"][0]["eboot_sha256"] = "0" * 64
            self.assertFalse(audit_inline(altered, root)["execution_prepared"])
            altered = copy.deepcopy(plan)
            altered["order"][-1]["selection_hash"] = "wrong"
            self.assertFalse(audit_inline(altered, root)["execution_prepared"])

    def test_blocker_mapping_fails_closed_if_matrix_changes(self):
        matrix = json.loads((ROOT / "conformance_inventory/dxvk_v262_matrix.json").read_text())
        changed = copy.deepcopy(matrix)
        next(row for row in changed["requirements"] if row["verdict"] == "satisfied")["verdict"] = "blocker"
        with self.assertRaises(ValueError):
            build_audit(matrix=changed)


if __name__ == "__main__":
    unittest.main()

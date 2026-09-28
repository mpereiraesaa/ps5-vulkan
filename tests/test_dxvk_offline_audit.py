"""Offline DXVK work queue must cover every blocker without promoting one."""
import copy
import hashlib
import json
import tempfile
import unittest

from pathlib import Path

from tools.audit_dxvk_offline import (
    ROOT, DOT_PACKAGE, INLINE_PLAN, REBUILT_WITNESSES, SIZE_MEASUREMENT, SIZE_PACKAGE, SIZE_PREVIOUS,
    T08_PACKAGE,
    ROBUST_IMAGE_PACKAGE,
    audit_inline, audit_rebuilt_witness, build_audit, selection_hash,
)


class DxvkOfflineAuditTests(unittest.TestCase):
    def test_current_audit_covers_all_17_and_checks_the_inline_artifacts(self):
        if not all((ROOT / path).is_file() for path in
                   (DOT_PACKAGE, INLINE_PLAN, SIZE_MEASUREMENT, SIZE_PACKAGE, SIZE_PREVIOUS,
                    T08_PACKAGE,
                    ROBUST_IMAGE_PACKAGE,
                    *REBUILT_WITNESSES.values())):
            self.skipTest("local offline candidate artifacts unavailable")
        report = build_audit()
        self.assertEqual(17, len(report["rows"]))
        self.assertEqual(17, len({r["id"] for r in report["rows"]}))
        self.assertEqual(8, report["inline_plan"]["checked_eboots"])
        self.assertTrue(report["inline_plan"]["execution_prepared"])
        self.assertTrue(report["subgroup_size_selection"]["selection_verified"])
        self.assertEqual(2, len(report["subgroup_size_selection"]["new_flagged_leaves"]))
        self.assertFalse(report["subgroup_size_selection"]["previous_candidate_covers_new_flags"])
        self.assertTrue(report["subgroup_size_package"]["package_verified"])
        self.assertTrue(report["subgroup_size_package"]["combined_cts_prepared"])
        self.assertEqual(6, report["subgroup_size_package"]["variant_count"])
        self.assertEqual(13, sum(r["phase"] == "native_validation" for r in report["rows"]))
        self.assertEqual({"computeFullSubgroups", "subgroupSizeControl"},
                         {r["name"] for r in report["rows"] if
                          r["name"] in {"computeFullSubgroups", "subgroupSizeControl"} and
                          r["phase"] == "native_validation"})
        self.assertTrue(report["integer_dot_package"]["package_verified"])
        self.assertTrue(report["robust_image_package"]["package_verified"])
        self.assertEqual(11, report["robust_image_package"]["variant_count"])
        self.assertEqual(8, report["robust_image_package"]["image_variants"])
        self.assertFalse(report["robust_image_package"]["original_cts_prepared"])
        self.assertEqual({"compute": 42, "graphics": 252}, report["integer_dot_package"]["variants"])
        self.assertTrue(report["t08_package"]["witnesses_verified"])
        self.assertTrue(report["t08_package"]["compiler_census_verified"])
        self.assertTrue(report["t08_package"]["cts_selection_verified"])
        self.assertTrue(report["t08_package"]["diagnostic_cts_prepared"])
        self.assertTrue(report["t08_package"]["eligible_cts_prepared"])
        self.assertTrue(report["t08_package"]["eligibility_gate_source_verified"])
        self.assertEqual(2, len(report["t08_package"]["excluded_format_gates"]))
        self.assertFalse(report["t08_package"]["original_cts_eligible"])
        self.assertTrue(all(r["execution_prepared"] and r["cts_verified"]
                            for r in report["rebuilt_witnesses"].values()))
        self.assertTrue(all(r["hardware_evidence_required"] for r in report["rows"]))

    def test_rebuilt_witness_audit_fails_closed_on_corrupt_eboot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate"
            (candidate / "PPSA99994").mkdir(parents=True)
            artifact_path = candidate / "artifact.json"
            eboot_path = candidate / "PPSA99994/eboot.bin"
            eboot_path.write_bytes(b"payload")
            digest = hashlib.sha256(eboot_path.read_bytes()).hexdigest()
            artifact_path.write_text(json.dumps({"eboot_sha256": digest}))
            record = {"candidate": "candidate", "source_commit": "a355320b",
                      "native_executed": False, "eboot_sha256": digest,
                      "artifact_sha256": hashlib.sha256(artifact_path.read_bytes()).hexdigest()}
            (root / "record.json").write_text(json.dumps(record))
            self.assertTrue(audit_rebuilt_witness(root, Path("record.json"))["artifact_verified"])
            eboot_path.write_bytes(b"changed")
            self.assertFalse(audit_rebuilt_witness(root, Path("record.json"))["execution_prepared"])

    def test_rebuilt_cts_requires_original_selection_and_diagnostic_switch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            witness = root / "candidate/PPSA99994"
            witness.mkdir(parents=True)
            witness_eboot = witness / "eboot.bin"
            witness_eboot.write_bytes(b"witness")
            witness_sha = hashlib.sha256(witness_eboot.read_bytes()).hexdigest()
            witness_artifact = root / "candidate/artifact.json"
            witness_artifact.write_text(json.dumps({"eboot_sha256": witness_sha}))
            base_cases = [{"path": "base"}]
            selected_cases = base_cases + [{"path": "original.cts.leaf"}]
            (root / "cts/upstream").mkdir(parents=True)
            (root / "cts/upstream/manifest.json").write_text(json.dumps({"cases": base_cases}))
            measurement = {"base_selection_hash": selection_hash(base_cases),
                           "selection_hash": selection_hash(selected_cases), "moved": 1}
            selection = {"cases": selected_cases, "measurement": measurement}
            (root / "selection.json").write_text(json.dumps(selection))
            cts = root / "cts-candidate/PPSA99994"
            cts.mkdir(parents=True)
            cts_eboot = cts / "eboot.bin"
            cts_eboot.write_bytes(b"cts")
            cts_sha = hashlib.sha256(cts_eboot.read_bytes()).hexdigest()
            build = {"eboot_sha256": cts_sha, "selection_hash": measurement["selection_hash"],
                     "selected_cases": [case["path"] for case in selected_cases],
                     "measurement": measurement,
                     "tessellation_build_profile": {"switches": {"DIAGNOSTIC": "1"}}}
            cts_manifest = cts / "build_manifest.json"
            cts_manifest.write_text(json.dumps(build))
            record = {"candidate": "candidate", "source_commit": "a355320b",
                      "native_executed": False, "eboot_sha256": witness_sha,
                      "artifact_sha256": hashlib.sha256(witness_artifact.read_bytes()).hexdigest(),
                      "cts": {"source_commit": "cb5c36b6", "candidate": "cts-candidate",
                              "eboot_sha256": cts_sha,
                              "build_manifest_sha256": hashlib.sha256(cts_manifest.read_bytes()).hexdigest(),
                              "selection_manifest": "selection.json",
                              "selection_hash": measurement["selection_hash"],
                              "diagnostic_switch": "DIAGNOSTIC", "moved": 1}}
            (root / "record.json").write_text(json.dumps(record))
            self.assertTrue(audit_rebuilt_witness(root, Path("record.json"))["cts_verified"])
            record["cts"]["diagnostic_switch"] = "WRONG"
            (root / "record.json").write_text(json.dumps(record))
            self.assertFalse(audit_rebuilt_witness(root, Path("record.json"))["cts_verified"])

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

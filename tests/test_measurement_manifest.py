import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from make_measurement_manifest import build_measurement_manifest, selection_hash  # noqa: E402


def _manifest():
    return {
        "manifest_version": "1.0",
        "cts_pin": {"commit": "abc"},
        "cases": [
            {"path": "dEQP-VK.a.one", "expected_status": "Pass"},
            {"path": "dEQP-VK.a.two", "expected_status": "Pass"},
        ],
        "diagnostics": [
            {"path": "dEQP-VK.b.measured", "category": "measured-pending",
             "expected_status": "Pass", "rationale": "runs on the measurement build"},
            {"path": "dEQP-VK.b.gap", "category": "known-gap",
             "expected_status": "NotSupported", "rationale": "documented gap"},
            {"path": "dEQP-VK.b.other", "category": "other-pending",
             "expected_status": "Pass", "rationale": "another family"},
        ],
    }


class MeasurementManifestTests(unittest.TestCase):
    def test_moves_only_the_named_pass_categories_and_records_the_derivation(self):
        frozen = _manifest()
        derived = build_measurement_manifest(frozen, {"measured-pending"}, "frozen.json")
        self.assertEqual(["dEQP-VK.a.one", "dEQP-VK.a.two", "dEQP-VK.b.measured"],
                         [c["path"] for c in derived["cases"]])
        self.assertEqual(["dEQP-VK.b.gap", "dEQP-VK.b.other"],
                         [d["path"] for d in derived["diagnostics"]])
        moved = derived["cases"][2]
        self.assertEqual("diagnostic:measured-pending", moved["measurement_origin"])
        self.assertEqual("Pass", moved["expected_status"])
        record = derived["measurement"]
        self.assertEqual("frozen.json", record["base_manifest"])
        self.assertEqual(selection_hash(frozen["cases"]), record["base_selection_hash"])
        self.assertEqual(selection_hash(derived["cases"]), record["selection_hash"])
        self.assertNotEqual(record["base_selection_hash"], record["selection_hash"])
        self.assertEqual(["measured-pending"], record["categories"])
        self.assertEqual(1, record["moved"])
        # The frozen input is not mutated.
        self.assertEqual(_manifest(), frozen)

    def test_refuses_categories_whose_oracle_is_not_expected_to_pass(self):
        with self.assertRaises(ValueError) as ctx:
            build_measurement_manifest(_manifest(), {"known-gap"})
        self.assertIn("NotSupported", str(ctx.exception))

    def test_refuses_unknown_or_empty_categories(self):
        with self.assertRaises(ValueError):
            build_measurement_manifest(_manifest(), set())
        with self.assertRaises(ValueError) as ctx:
            build_measurement_manifest(_manifest(), {"measured-pending", "nope"})
        self.assertIn("nope", str(ctx.exception))

    def test_frozen_manifest_derives_the_t05_measurement_selection(self):
        frozen = json.loads((ROOT / "cts/upstream/manifest.json").read_text())
        # The T05 categories are gone from the frozen manifest: those leaves
        # were measured and promoted into the acceptance selection, so the
        # derivation is exercised over whatever categories the manifest still
        # holds. A manifest with nothing left to move has nothing to derive,
        # and the tool says so rather than inventing a selection.
        categories = {d["category"] for d in frozen["diagnostics"]
                      if d.get("expected_status") == "Pass"}
        if not categories:
            with self.assertRaises(ValueError):
                build_measurement_manifest(frozen, {"rasterization-culling"})
            return
        derived = build_measurement_manifest(frozen, categories)
        self.assertEqual(len(frozen["cases"]) + derived["measurement"]["moved"],
                         len(derived["cases"]))
        self.assertTrue(all(c["expected_status"] == "Pass" for c in derived["cases"]))
        paths = [c["path"] for c in derived["cases"]]
        self.assertEqual(len(paths), len(set(paths)))

    def test_frozen_manifest_contains_promoted_bda_selection(self):
        frozen = json.loads((ROOT / "cts/upstream/manifest.json").read_text())
        prefix = ("dEQP-VK.binding_model.buffer_device_address."
                  "set0.depth1.basessbo.load.nostore.single.std140.")
        expected = {prefix + "comp", prefix + "comp_offset_nonzero"}
        selected = [c for c in frozen["cases"]
                    if c["category"] == "t08-buffer-device-address-base"]
        self.assertEqual(expected, {c["path"] for c in selected})
        self.assertEqual(507, len(frozen["cases"]))
        self.assertEqual(111, len(frozen["diagnostics"]))
        self.assertEqual("d93a2cb2ea282924c57c63f1412cddd8c22cd799b549fb4cdcbbecd6ce73c321",
                         selection_hash(frozen["cases"]))
        self.assertFalse(expected & {d["path"] for d in frozen["diagnostics"]})

    def test_cli_writes_the_manifest_the_builder_and_runner_take(self):
        with tempfile.TemporaryDirectory() as tmp:
            frozen = Path(tmp) / "frozen.json"
            frozen.write_text(json.dumps(_manifest()))
            out = Path(tmp) / "sub" / "measurement.json"
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/make_measurement_manifest.py"),
                 "--manifest", str(frozen), "--category", "measured-pending",
                 "--category", "other-pending", "-o", str(out)],
                capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertIn("2 leaves moved", result.stdout)
            written = json.loads(out.read_text())
            self.assertEqual(4, len(written["cases"]))
            self.assertEqual(["dEQP-VK.b.gap"], [d["path"] for d in written["diagnostics"]])
            self.assertEqual(2, written["measurement"]["moved"])


if __name__ == "__main__":
    unittest.main()
